#ifdef ENABLE_OPENGL

#include <stdint.h>
#include <stdbool.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#ifdef __MINGW32__
#define FOR_WINDOWS 1
#else
#define FOR_WINDOWS 0
#endif

#if FOR_WINDOWS
#include <GL/glew.h>
#include "SDL.h"
#define GL_GLEXT_PROTOTYPES 1
#include "SDL_opengl.h"
#else
#include <SDL2/SDL.h>
//#define GL_GLEXT_PROTOTYPES 1
#include <SDL2/SDL_opengles2.h>
#endif

static int has_vao_support = 0;
static PFNGLBINDVERTEXARRAYOESPROC glBindVertexArrayOES;
static PFNGLGENVERTEXARRAYSOESPROC glGenVertexArraysOES;

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "gfx_opengl_dynares.h"
#include "../cheapProfiler.h"

struct ShaderProgram {
    uint32_t shader_id;
    GLuint opengl_program_id;
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_floats;
    GLint attrib_locations[12];
    uint8_t attrib_sizes[12];
    uint8_t num_attribs;
    bool used_noise;
    GLint frame_count_location;
    GLint window_height_location;
    GLuint vao;
    bool init;
};

struct FBOBlitter {
    GLuint program;
    GLuint vao;
    GLuint fbo, fbo_tex, fbo_depth;
    GLfloat h_scale, v_scale;
    GLuint width, height;
    GLint aCoord;
    GLint uScale;
    GLint uFBOTex;
    int status;
};

static struct ShaderProgram shader_program_pool[64];
static uint8_t shader_program_pool_size;
static GLuint opengl_vbo;

static uint32_t frame_count;
static uint32_t current_height;

static struct FBOBlitter dynares = {};

static GLuint gfx_compile_shaders(const GLchar *sources[2], const const GLint lengths[2])
{
    GLint success;
    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &sources[0], &lengths[0]);
    glCompileShader(vertex_shader);
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint max_length = 0;
        glGetShaderiv(vertex_shader, GL_INFO_LOG_LENGTH, &max_length);
        char error_log[1024];
        fprintf(stderr, "Vertex shader compilation failed\n");
        fprintf(stderr, "================================\n");
        fprintf(stderr, sources[0]);
        fprintf(stderr, "================================\n");
        glGetShaderInfoLog(vertex_shader, max_length, &max_length, &error_log[0]);
        fprintf(stderr, "%s\n", &error_log[0]);
        abort();
    }

    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &sources[1], &lengths[1]);
    glCompileShader(fragment_shader);
    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint max_length = 0;
        glGetShaderiv(fragment_shader, GL_INFO_LOG_LENGTH, &max_length);
        char error_log[1024];
        fprintf(stderr, "Fragment shader compilation failed\n");
        fprintf(stderr, "================================\n");
        fprintf(stderr, sources[1]);
        fprintf(stderr, "================================\n");
        glGetShaderInfoLog(fragment_shader, max_length, &max_length, &error_log[0]);
        fprintf(stderr, "%s\n", &error_log[0]);
        abort();
    }

    GLuint shader_program = glCreateProgram();
    glAttachShader(shader_program, vertex_shader);
    glAttachShader(shader_program, fragment_shader);
    glLinkProgram(shader_program);
    return shader_program;
}

#ifdef USE_TEXTURE_ATLAS
GLuint vt_page;
#endif

static bool gfx_opengl_z_is_from_0_to_1(void) {
    return false;
}

static void gfx_opengl_vertex_array_set_attribs(struct ShaderProgram *prg) {
    size_t num_floats = prg->num_floats;
    size_t pos = 0;

    for (int i = 0; i < prg->num_attribs; i++) {
        glEnableVertexAttribArray(prg->attrib_locations[i]);
        glVertexAttribPointer(prg->attrib_locations[i], prg->attrib_sizes[i], GL_FLOAT, GL_FALSE, num_floats * sizeof(float), (void *) (pos * sizeof(float)));
        pos += prg->attrib_sizes[i];
    }
}

static void gfx_opengl_set_uniforms(struct ShaderProgram *prg) {
    if (prg->used_noise) {
        glUniform1i(prg->frame_count_location, frame_count);
        glUniform1i(prg->window_height_location, current_height);
    }
}

static void gfx_opengl_unload_shader(struct ShaderProgram *old_prg) {
    if (has_vao_support)
        return;
    
    if (old_prg != NULL) {
        for (int i = 0; i < old_prg->num_attribs; i++) {
            glDisableVertexAttribArray(old_prg->attrib_locations[i]);
        }
    }
}

static void gfx_opengl_load_shader(struct ShaderProgram *new_prg) {
    glUseProgram(new_prg->opengl_program_id);
    if (has_vao_support) {
        if (!new_prg->init) {
            new_prg->init = 1;
            glGenVertexArraysOES(1, &new_prg->vao);
            glBindVertexArrayOES(new_prg->vao);
            gfx_opengl_vertex_array_set_attribs(new_prg);
        }
        else {
            glBindVertexArrayOES(new_prg->vao);
        }
    } else {
        gfx_opengl_vertex_array_set_attribs(new_prg);
    }

    gfx_opengl_set_uniforms(new_prg);
}

static void append_str(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
}

static void append_line(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
    buf[(*len)++] = '\n';
}

static const char *shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "vec4(0.0, 0.0, 0.0, 0.0)" : "vec3(0.0, 0.0, 0.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_TEXEL0:
                return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.a" :
                    (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)" : "vec3(texVal0.a, texVal0.a, texVal0.a)");
            case SHADER_TEXEL1:
                return with_alpha ? "texVal1" : "texVal1.rgb";
        }
    } else {
        switch (item) {
            case SHADER_0:
                return "0.0";
            case SHADER_INPUT_1:
                return "vInput1.a";
            case SHADER_INPUT_2:
                return "vInput2.a";
            case SHADER_INPUT_3:
                return "vInput3.a";
            case SHADER_INPUT_4:
                return "vInput4.a";
            case SHADER_TEXEL0:
                return "texVal0.a";
            case SHADER_TEXEL0A:
                return "texVal0.a";
            case SHADER_TEXEL1:
                return "texVal1.a";
        }
    }
}

static void append_formula(char *buf, size_t *len, uint8_t c[2][4], bool do_single, bool do_multiply, bool do_mix, bool with_alpha, bool only_alpha, bool opt_alpha) {
    if (do_single) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    } else if (do_multiply) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
    } else if (do_mix) {
        append_str(buf, len, "mix(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, ")");
    } else {
        append_str(buf, len, "(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " - ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ") * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, " + ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    }
}

static struct ShaderProgram *gfx_opengl_create_and_load_new_shader(uint32_t shader_id) {
    struct CCFeatures cc_features;
    gfx_cc_get_features(shader_id, &cc_features);

    char vs_buf[4096];
    char fs_buf[4096];
    size_t vs_len = 0;
    size_t fs_len = 0;
    size_t num_floats = 4;

    int num_samplers = cc_features.used_textures[0] + cc_features.used_textures[1];
    char *aTexParams_type[] = {
        "",
        "vec2",
        "vec4"
    };

    // Vertex shader
#ifndef USE_GLES2
    append_line(vs_buf, &vs_len, "#version 110");
#else
    append_line(vs_buf, &vs_len, "#version 100");
#endif
    append_line(vs_buf, &vs_len, "precision highp float;");
    append_line(vs_buf, &vs_len, "attribute vec4 aVtxPos;");
    if (cc_features.used_textures[0] || cc_features.used_textures[1]) {
        append_line(vs_buf, &vs_len, "attribute vec2 aTexCoord;");
#ifndef USE_TEXTURE_ATLAS
        append_line(vs_buf, &vs_len, "varying vec2 vTexCoord;");
#else
        vs_len += sprintf(vs_buf + vs_len, "#define bundle_t %s\n", aTexParams_type[num_samplers]);
        vs_len += sprintf(vs_buf + vs_len, "attribute bundle_t aTexParams;\n");
        for (int i = 0, samplers = 1; i < 2; i++) {
            if (cc_features.used_textures[i]) {
                vs_len += sprintf(vs_buf + vs_len, "varying vec4 vTexDimensions%d;\n", samplers);
                vs_len += sprintf(vs_buf + vs_len, "varying vec4 vTexSampler%d;\n", samplers);
                vs_len += sprintf(vs_buf + vs_len, "varying vec2 vTexCoord%d;\n", samplers);
                samplers++;
                num_floats += 2;
            }
        }
#endif
        num_floats += 2;
    }
    if (cc_features.opt_fog) {
        append_line(vs_buf, &vs_len, "attribute vec4 aFog;");
        append_line(vs_buf, &vs_len, "varying vec4 vFog;");
        num_floats += 4;
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        vs_len += sprintf(vs_buf + vs_len, "attribute vec%d aInput%d;\n", cc_features.opt_alpha ? 4 : 3, i + 1);
        vs_len += sprintf(vs_buf + vs_len, "varying vec%d vInput%d;\n", cc_features.opt_alpha ? 4 : 3, i + 1);
        num_floats += cc_features.opt_alpha ? 4 : 3;
    }

#ifdef USE_TEXTURE_ATLAS
    // Returns two texture param activator tuples.
    // e.g.: (is_mirror0, is_clamp0, is_mirror1, is_clamp1)
    // Normal Repeat operation is implied is_repeat = 1-(is_mirror0+is_clamp0) 
    append_line(vs_buf, &vs_len,
        "vec4 cms_cmt(vec2 cmst) {"
        "   vec4 temp = cmst.xxyy;"
        "   return 1.0 - step(0.1, abs(temp - vec4(2.0, 1.0, 2.0, 1.0)));"
        "}"
    );
#endif
    
    append_line(vs_buf, &vs_len, "void main() {");
#ifndef USE_TEXTURE_ATLAS
    if (cc_features.used_textures[0] || cc_features.used_textures[1]) {
        append_line(vs_buf, &vs_len, "vTexCoord = aTexCoord;");
    }
#endif
    if (cc_features.opt_fog) {
        append_line(vs_buf, &vs_len, "vFog = aFog;");
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        vs_len += sprintf(vs_buf + vs_len, "vInput%d = aInput%d;\n", i + 1, i + 1);
    }

    // Extract the bundled encFloat_t in parallel
    if (num_samplers > 0) {
        // exponent = floor(log2(value))
        append_line(vs_buf, &vs_len, "bundle_t e = floor(log2(aTexParams));");
        append_line(vs_buf, &vs_len, "bundle_t p = pow(bundle_t(2.0), e);");
        // MAXIMUM_MANTISSA_VALUE = (1<<23)-1 = 8388607.0
        // mantissa = MAXIMUM_MANTISSA_VALUE * (value / 2.0**exponent)) - 1)
        append_line(vs_buf, &vs_len, "bundle_t mant = 8388607.0 * ((aTexParams / p) - bundle_t(1.0));");
        // cmst = ((e+BIAS) >> 1)
        append_line(vs_buf, &vs_len, "bundle_t dec_cmst = floor((e + 127.0) / 2.0);");
        // width or height = mant >> 11
        append_line(vs_buf, &vs_len, "bundle_t dec_zw = floor(mant / 2048.0);");
        // x or y = mant & 0xFFF
        append_line(vs_buf, &vs_len, "bundle_t dec_xy = mant - (dec_zw * 2048.0);");

        // Swizzle all the necessary things into their correct places
        if (num_samplers > 0) {
            // Dimensions are in the [0-2048] range, but OpenGL expects [0-1]. Scale it down.
            // Johnny: I don't see any point in using an uniform for the texture range
            // since we only have 11 bits of precision on the encoded numbers. 
            // There's an extra 2 unused on the exponential, but first let's attempt not to use them.
            append_line(vs_buf, &vs_len, "vTexDimensions1 = vec4(dec_xy.xy, dec_zw.xy);");
            append_line(vs_buf, &vs_len, "vTexDimensions1 = vTexDimensions1 / 2048.0;");
            append_line(vs_buf, &vs_len, "vTexSampler1 = cms_cmt(dec_cmst.xy);");

            // In case we have a mirrored tile, we'll used the pre-uploaded mirrors
            // For that, we'll fix the dimensions here by pre-multiplying them
            append_line(vs_buf, &vs_len, "vTexDimensions1.zw *= vTexSampler1.yw + 1.0;"); // Use mirrored images
            append_line(vs_buf, &vs_len, "vTexCoord1 = aTexCoord / (vTexSampler1.yw + 1.0);"); // Use mirrored images
            if (num_samplers == 2) {
                append_line(vs_buf, &vs_len, "vTexDimensions2 = vec4(dec_xy.zw, dec_zw.zw);");
                append_line(vs_buf, &vs_len, "vTexDimensions2 = vTexDimensions2 / 2048.0;");
                append_line(vs_buf, &vs_len, "vTexSampler2 = cms_cmt(dec_cmst.zw);");

                // Same here
                append_line(vs_buf, &vs_len, "vTexDimensions2.zw *= vTexSampler2.yw + 1.0;"); // Use mirrored images
                append_line(vs_buf, &vs_len, "vTexCoord2 = aTexCoord / (vTexSampler2.yw + 1.0);"); // Use mirrored images
            }
        }
    }    

    append_line(vs_buf, &vs_len, "gl_Position = aVtxPos;");
    append_line(vs_buf, &vs_len, "}");

    // Fragment shader
#ifndef USE_GLES2
    append_line(fs_buf, &fs_len, "#version 110");
#else
    append_line(fs_buf, &fs_len, "#version 100");
    append_line(fs_buf, &fs_len, "precision highp float;");
#endif
    //append_line(fs_buf, &fs_len, "precision mediump float;");
    if (cc_features.used_textures[0] || cc_features.used_textures[1]) {
#ifndef USE_TEXTURE_ATLAS
        append_line(fs_buf, &fs_len, "varying vec2 vTexCoord;");
#else
        for (int i = 0, samplers = 1; i < 2; i++) {
            if (cc_features.used_textures[i]) {
                fs_len += sprintf(fs_buf + fs_len, "varying vec4 vTexDimensions%d;\n", samplers);
                fs_len += sprintf(fs_buf + fs_len, "varying vec4 vTexSampler%d;\n", samplers);
                fs_len += sprintf(fs_buf + fs_len, "varying vec2 vTexCoord%d;\n", samplers);
                samplers++;
            }
        }
#endif
    }

    if (cc_features.opt_fog) {
        append_line(fs_buf, &fs_len, "varying vec4 vFog;");
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        fs_len += sprintf(fs_buf + fs_len, "varying vec%d vInput%d;\n", cc_features.opt_alpha ? 4 : 3, i + 1);
    }
    if (cc_features.used_textures[0]) {
        append_line(fs_buf, &fs_len, "uniform sampler2D uTex0;");
    }
    if (cc_features.used_textures[1]) {
        append_line(fs_buf, &fs_len, "uniform sampler2D uTex1;");
    }

#ifndef USE_GLES2
    if (cc_features.opt_alpha && cc_features.opt_noise) {
        append_line(fs_buf, &fs_len, "uniform int frame_count;");
        append_line(fs_buf, &fs_len, "uniform int window_height;");

        append_line(fs_buf, &fs_len, "float random(in vec3 value) {");
        append_line(fs_buf, &fs_len, "    float random = dot(sin(value), vec3(12.9898, 78.233, 37.719));");
        append_line(fs_buf, &fs_len, "    return fract(sin(random) * 143758.5453);");
        append_line(fs_buf, &fs_len, "}");
    }
#endif

    append_line(fs_buf, &fs_len, "void main() {");

#ifndef USE_TEXTURE_ATLAS
    if (cc_features.used_textures[0]) {
        append_line(fs_buf, &fs_len, "vec4 texVal0 = texture2D(uTex0, vTexCoord);");
    }
    if (cc_features.used_textures[1]) {
        append_line(fs_buf, &fs_len, "vec4 texVal1 = texture2D(uTex1, vTexCoord);");
    }
#else
    if (num_samplers > 0) {
        append_line(fs_buf, &fs_len, "vec2 texCoords;");

        // See the definition of cms_cmt() to understand this.
        // We use precomputed sampler activators here to avoid calculating them 
        // on the fragment shaders, and to allow us to simd the coordinate params.
        for (int i = 1; i <= num_samplers; i++) {
            if (cc_features.used_textures[i-1]) {
                fs_len += sprintf(fs_buf + fs_len, "texCoords = vTexDimensions%d.xy;", i);
                fs_len += sprintf(fs_buf + fs_len, "texCoords +=      vTexSampler%d.xz  * vTexDimensions%d.zw * clamp(vTexCoord%d, 0.0, 1.0);", i, i, i);
                fs_len += sprintf(fs_buf + fs_len, "texCoords += (1.0-vTexSampler%d.xz) * vTexDimensions%d.zw * fract(vTexCoord%d);", i, i, i);
                fs_len += sprintf(fs_buf + fs_len, "vec4 texVal%d = texture2D(uTex%d, texCoords);", i-1, i-1);
            }
        }
    }
#endif

    append_str(fs_buf, &fs_len, cc_features.opt_alpha ? "vec4 texel = " : "vec3 texel = ");
    if (!cc_features.color_alpha_same && cc_features.opt_alpha) {
        append_str(fs_buf, &fs_len, "vec4(");
        append_formula(fs_buf, &fs_len, cc_features.c, cc_features.do_single[0], cc_features.do_multiply[0], cc_features.do_mix[0], false, false, true);
        append_str(fs_buf, &fs_len, ", ");
        append_formula(fs_buf, &fs_len, cc_features.c, cc_features.do_single[1], cc_features.do_multiply[1], cc_features.do_mix[1], true, true, true);
        append_str(fs_buf, &fs_len, ")");
    } else {
        append_formula(fs_buf, &fs_len, cc_features.c, cc_features.do_single[0], cc_features.do_multiply[0], cc_features.do_mix[0], cc_features.opt_alpha, false, cc_features.opt_alpha);
    }
    append_line(fs_buf, &fs_len, ";");


    if (cc_features.opt_texture_edge && cc_features.opt_alpha) {
        append_line(fs_buf, &fs_len, "if (texel.a > 0.3) texel.a = 1.0; else discard;");
    }

    // TODO discard if alpha is 0?
    if (cc_features.opt_fog) {
        if (cc_features.opt_alpha) {
            append_line(fs_buf, &fs_len, "texel = vec4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);");
        } else {
            append_line(fs_buf, &fs_len, "texel = mix(texel, vFog.rgb, vFog.a);");
        }
    }

#ifndef USE_GLES2
    if (cc_features.opt_alpha && cc_features.opt_noise) {
        append_line(fs_buf, &fs_len, "texel.a *= floor(random(vec3(floor(gl_FragCoord.xy * (240.0 / float(window_height))), float(frame_count))) + 0.5);");
    }
#endif

    if (cc_features.opt_alpha) {
        append_line(fs_buf, &fs_len, "gl_FragColor = texel;");
    } else {
        append_line(fs_buf, &fs_len, "gl_FragColor = vec4(texel, 1.0);");
    }
    append_line(fs_buf, &fs_len, "}");

    vs_buf[vs_len] = '\0';
    fs_buf[fs_len] = '\0';

    const GLchar *sources[2] = { vs_buf, fs_buf };
    const GLint lengths[2] = { vs_len, fs_len };
    GLuint shader_program = gfx_compile_shaders(sources, lengths);

    size_t cnt = 0;

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];
    prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aVtxPos");
    prg->attrib_sizes[cnt] = 4;
    ++cnt;

    if (cc_features.used_textures[0] || cc_features.used_textures[1]) {
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aTexCoord");
        prg->attrib_sizes[cnt] = 2;
        ++cnt;
#ifdef USE_TEXTURE_ATLAS
        if (num_samplers > 0) {
            prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aTexParams");
            prg->attrib_sizes[cnt] = num_samplers * 2; /* vec2 or vec4 */
            ++cnt;
        }
#endif
    }

    if (cc_features.opt_fog) {
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aFog");
        prg->attrib_sizes[cnt] = 4;
        ++cnt;
    }

    for (int i = 0; i < cc_features.num_inputs; i++) {
        char name[16];
        sprintf(name, "aInput%d", i + 1);
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, name);
        prg->attrib_sizes[cnt] = cc_features.opt_alpha ? 4 : 3;
        ++cnt;
    }

    prg->shader_id = shader_id;
    prg->opengl_program_id = shader_program;
    prg->num_inputs = cc_features.num_inputs;
    prg->used_textures[0] = cc_features.used_textures[0];
    prg->used_textures[1] = cc_features.used_textures[1];
    prg->num_floats = num_floats;
    prg->num_attribs = cnt;
    prg->init = 0;

    gfx_opengl_load_shader(prg);

    if (cc_features.used_textures[0]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex0");
        glUniform1i(sampler_location, 0);
    }
    if (cc_features.used_textures[1]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex1");
        glUniform1i(sampler_location, 1);
    }

    if (cc_features.opt_alpha && cc_features.opt_noise) {
        prg->frame_count_location = glGetUniformLocation(shader_program, "frame_count");
        prg->window_height_location = glGetUniformLocation(shader_program, "window_height");
        prg->used_noise = true;
    } else {
        prg->used_noise = false;
    }

    return prg;
}

static struct ShaderProgram *gfx_opengl_lookup_shader(uint32_t shader_id) {
    for (size_t i = 0; i < shader_program_pool_size; i++) {
        if (shader_program_pool[i].shader_id == shader_id) {
            return &shader_program_pool[i];
        }
    }
    return NULL;
}

static void gfx_opengl_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static GLuint gfx_opengl_new_texture(void) {
    GLuint ret;
    glGenTextures(1, &ret);
    return ret;
}

static void gfx_opengl_select_texture(int tile, GLuint texture_id) {
    ProfEmitEventStart("glBindTexture");
    glActiveTexture(GL_TEXTURE0 + tile);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    ProfEmitEventEnd("glBindTexture");
}

static void gfx_opengl_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    ProfEmitEventStart("glTexImage2D");
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba32_buf);
    ProfEmitEventEnd("glTexImage2D");
}

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    if (val & G_TX_CLAMP) {
        return GL_CLAMP_TO_EDGE;
    }
    return (val & G_TX_MIRROR) ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

static void gfx_opengl_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    ProfEmitEventStart("gfx_opengl_set_sampler_parameters");
    glActiveTexture(GL_TEXTURE0 + tile);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear_filter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear_filter ? GL_LINEAR : GL_NEAREST);
#ifndef USE_TEXTURE_ATLAS
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gfx_cm_to_opengl(cms));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gfx_cm_to_opengl(cmt));
#endif
    ProfEmitEventEnd("gfx_opengl_set_sampler_parameters");
}

static void gfx_opengl_set_depth_test(bool depth_test) {
    if (depth_test) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
}

static void gfx_opengl_set_depth_mask(bool z_upd) {
    glDepthMask(z_upd ? GL_TRUE : GL_FALSE);
}

static void gfx_opengl_set_zmode_decal(bool zmode_decal) {
    if (zmode_decal) {
        glPolygonOffset(-2, -2);
        glEnable(GL_POLYGON_OFFSET_FILL);
    } else {
        glPolygonOffset(0, 0);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
}

static void gfx_opengl_set_viewport(int x, int y, int width, int height) {
    if (dynares.status > 0) {
        x *= dynares.h_scale;
        y *= dynares.v_scale;
        width *= dynares.h_scale;
        height *= dynares.v_scale;
    }

    glViewport(x, y, width, height);
    current_height = height;
}

static void gfx_opengl_set_scissor(int x, int y, int width, int height) {
    if (dynares.status > 0) {
        x *= dynares.h_scale;
        y *= dynares.v_scale;
        width *= dynares.h_scale;
        height *= dynares.v_scale;
    }

    glScissor(x, y, width, height);
}

static void gfx_opengl_set_use_alpha(bool use_alpha) {
    if (use_alpha) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
}

static void gfx_opengl_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    //printf("flushing %d tris\n", buf_vbo_num_tris);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * buf_vbo_len, buf_vbo, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 3 * buf_vbo_num_tris);
}

static void gfx_opengl_init(void) {
#if FOR_WINDOWS
    glewInit();
#endif
#ifdef USE_GLES2
    glGenVertexArraysOES = SDL_GL_GetProcAddress("glGenVertexArraysOES");
    glBindVertexArrayOES = SDL_GL_GetProcAddress("glBindVertexArrayOES");
#else
    glGenVertexArraysOES = SDL_GL_GetProcAddress("glGenVertexArrays");
    glBindVertexArrayOES = SDL_GL_GetProcAddress("glBindVertexArray");
#endif
    if (!glGenVertexArraysOES || !glBindVertexArrayOES) {
        printf("Missing GL_OES_vertex_array_object, falling back.\n");
    } else {
        has_vao_support = 1;
    }

    glGenBuffers(1, &opengl_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
    
    glDepthFunc(GL_LEQUAL);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void gfx_opengl_on_resize(void) {
}

static void gfx_opengl_start_frame() {
    dynares.h_scale = 0.5f;
    dynares.v_scale = 0.5f;

    gfx_opengl_bind_dynares(dynares.width, dynares.height);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE); // Must be set to clear Z-buffer
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
}

static void gfx_opengl_end_frame(void) {
    ProfEmitEventStart("gfx_opengl_swap_dynares");
    if (dynares.status > 0) {
        gfx_opengl_swap_dynares();
    }

    ProfEmitEventEnd("gfx_opengl_swap_dynares");
}

static void gfx_opengl_finish_render(void) {
}

#ifdef USE_TEXTURE_ATLAS
static void gfx_opengl_bind_virtual_texture_page(void)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vt_page);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, vt_page);
}

static void gfx_opengl_create_virtual_texture_page(uint16_t dimensions)
{
    glGenTextures(1, &vt_page);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vt_page);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, dimensions, dimensions, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

static void mirror_horizontal(uint32_t *mirror_buf, uint32_t *rgba32_buf, int width, int height)
{
    uint32_t *src = &rgba32_buf[0];
    uint32_t *dst_orig = &mirror_buf[0];
    uint32_t *dst_mror = &mirror_buf[width*2 + 2];

    for (int i = 0; i < height; i++) {
        /* Fetch for left/right borders */
        uint32_t fetch = *src;
        
        *dst_orig++ = fetch;
        *dst_mror-- = fetch;

        for (int j = 0; j < width; j++) {
            // Fill inner buffers
            fetch = *src++;
            *dst_orig++ = fetch;
            *dst_mror-- = fetch;
        }
        dst_orig +=  width + 1;      /* line pitch + border   */
        dst_mror += (width + 1) * 3; /* same as above, but 3x */
    }
}

static void mirror_vertical(uint32_t *mirror_buf, uint32_t *rgba32_buf, int width, int height)
{
    // Virtual Stride - or "stride with added borders"
    uint32_t v_stride = width + 2;
    uint32_t *src = &rgba32_buf[0];
    uint32_t *dst = &mirror_buf[v_stride * (height * 2)];

    for (int i = 0; i < height; i++) {
        uint32_t fetch;
        *dst-- = *src;
        for (int j = 0; j < width; j++) {
            fetch = *src++;
            *dst-- = fetch;
        }
        *dst-- = fetch;
    }
}

static void mirror_both(uint32_t *mirror_buf, uint32_t *rgba32_buf, int width, int height)
{
    // Usual horizontal mirroring
    mirror_horizontal(mirror_buf, rgba32_buf, width, height);

    // Virtual Stride - or "stride with added borders"
    uint32_t v_stride = width*2 + 2;
    uint32_t *src = &mirror_buf[0];
    uint32_t *dst = &mirror_buf[v_stride * (height * 2)];

    // Border-less vertical mirroring
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < v_stride; j++) {
            *dst-- = *src++;
        }
    }
}

static void only_borders(uint32_t *mirror_buf, uint32_t *rgba32_buf, int width, int height)
{
    uint32_t *src = &rgba32_buf[0];
    uint32_t *dst = &mirror_buf[0];

    for (int i = 0; i < height; i++) {
        *dst++ = *src;
        for (int j = 0; j < width; j++) {
            *dst++ = *src++;
        }
        *dst++ = *(src-1);
    }
}

static void gfx_opengl_upload_virtual_texture(const uint8_t *rgba32_buf, int x, 
    int y, int width, int height, int h_mirror, int v_mirror)
{
    ProfEmitEventStart("gfx_opengl_upload_virtual_texture");
    int has_mirrors = h_mirror || v_mirror;

    // Full size for mirror + 3x borders
    uint32_t mirror_buf[4096 * 4 + ((63-1) * 4)];

    int v_stride = 2 + ((!h_mirror) ? width  : width  * 2);
    int v_height = 2 + ((!v_mirror) ? height : height * 2);
    uint32_t *mirror_buf_head = mirror_buf + v_height * v_stride; 

    if (h_mirror && v_mirror) mirror_both(&mirror_buf[v_stride], rgba32_buf, width, height);
    else if (h_mirror)  mirror_horizontal(&mirror_buf[v_stride], rgba32_buf, width, height);
    else if (v_mirror)    mirror_vertical(&mirror_buf[v_stride], rgba32_buf, width, height);
    else                     only_borders(&mirror_buf[v_stride], rgba32_buf, width, height);

    // Create the top and bottom mirrors, respectively.
    memcpy(mirror_buf, mirror_buf + v_stride, v_stride * sizeof(uint32_t));
    memcpy(mirror_buf_head - v_stride, mirror_buf_head - v_stride*2, v_stride * sizeof(uint32_t));

    // Upload texture page
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vt_page);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x - 1, y - 1, v_stride, v_height, GL_RGBA, GL_UNSIGNED_BYTE, mirror_buf);

    ProfEmitEventEnd("gfx_opengl_upload_virtual_texture");
}
#endif

void gfx_opengl_bind_dynares(uint32_t width, uint32_t height)
{
    if (dynares.status <= 0)
        return;

    glBindFramebuffer(GL_FRAMEBUFFER, dynares.fbo);
    glViewport(0, 0, width * dynares.h_scale, height * dynares.v_scale);
}

void gfx_opengl_swap_dynares()
{
    if (dynares.status <= 0)
        return;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, dynares.width, dynares.height);

    glUseProgram(dynares.program);
    if (has_vao_support) {
        glBindVertexArrayOES(dynares.vao);
    } else {
        glEnableVertexAttribArray(dynares.aCoord);
        glVertexAttribPointer(dynares.aCoord, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), 0);
    }

    // Bind texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, dynares.fbo_tex);

    // Set uniforms
    GLfloat h_scale = floor(dynares.width * dynares.h_scale) / dynares.width;
    GLfloat v_scale = floor(dynares.height * dynares.v_scale) / dynares.height;
    glUniform2f(dynares.uScale, h_scale, v_scale);
    glUniform1i(dynares.uFBOTex, 0);

    // Dispatch drawcall
    GLfloat vert[] = {
        -1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof(vert), vert, GL_STREAM_DRAW);    
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Undo VA
    if (has_vao_support) {
        glBindVertexArrayOES(0);
    } else {
        glDisableVertexAttribArray(dynares.aCoord);
    }
}

int gfx_opengl_init_dynares_program(void)
{
    // Create Program
    GLchar *sources[2] = {gfx_dynares_vshader, gfx_dynares_fshader};
    GLint lengths[2] = {strlen(gfx_dynares_vshader), strlen(gfx_dynares_fshader)};
    dynares.program = gfx_compile_shaders(sources, lengths);

    dynares.aCoord = glGetAttribLocation(dynares.program, "aCoord");
    dynares.uScale = glGetUniformLocation(dynares.program, "uScale");
    dynares.uFBOTex = glGetUniformLocation(dynares.program, "uFBOTex");

    // Prepare the vertex array object
    if (has_vao_support) {
        glGenVertexArraysOES(1, &dynares.vao);
        glBindVertexArrayOES(dynares.vao);
        glEnableVertexAttribArray(dynares.aCoord);
        glVertexAttribPointer(dynares.aCoord, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), 0);
        glBindVertexArrayOES(0);
    }
}

void gfx_opengl_init_dynares(uint32_t width, uint32_t height)
{
    // TODO:: REMOVE THIS
    // DYNAMIC RES IS A HACK UNTIL FURTHER DISCUSSION AND TESTS
    if (width <= 320)
        return;

    // Failed for some reason, don't retry
    if (dynares.status < 0)
        return;

    // We are initialized and don't need updating it.
    if (dynares.status > 0 && dynares.width == width && dynares.height == height)
        return;

    // We are initializing for the first time, populate necessary data.
    if (dynares.status == 0) {
        gfx_opengl_init_dynares_program();
        glGenFramebuffers(1, &dynares.fbo);
        glGenTextures(1, &dynares.fbo_tex);
        glGenRenderbuffers(1, &dynares.fbo_depth);

        dynares.h_scale = 1.0;
        dynares.v_scale = 1.0;
    }

    // Bind the new framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, dynares.fbo);

    // Allocate fbo texture and bind it to fbo
    glBindTexture(GL_TEXTURE_2D, dynares.fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dynares.fbo_tex, 0);    

    // Allocate fbo depth buffer and bind it to fbo
    glBindRenderbuffer(GL_RENDERBUFFER, dynares.fbo_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, dynares.fbo_depth);

    // Store FB status
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    // Unbind the new framebuffer, for cleanliness.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        printf("Framebuffer status incomplete, 0x%04x\n", status);
        dynares.status = -1;
        return;
    }

    dynares.width = width;
    dynares.height = height;
    dynares.status = 1;
}

static void gfx_opengl_signal_start(uint32_t width, uint32_t height)
{
    gfx_opengl_init_dynares(width, height);
}

struct GfxRenderingAPI gfx_opengl_api = {
    gfx_opengl_z_is_from_0_to_1,
    gfx_opengl_unload_shader,
    gfx_opengl_load_shader,
    gfx_opengl_create_and_load_new_shader,
    gfx_opengl_lookup_shader,
    gfx_opengl_shader_get_info,
    gfx_opengl_new_texture,
    gfx_opengl_select_texture,
    gfx_opengl_upload_texture,
    gfx_opengl_set_sampler_parameters,
    gfx_opengl_set_depth_test,
    gfx_opengl_set_depth_mask,
    gfx_opengl_set_zmode_decal,
    gfx_opengl_set_viewport,
    gfx_opengl_set_scissor,
    gfx_opengl_set_use_alpha,
    gfx_opengl_draw_triangles,
    gfx_opengl_init,
    gfx_opengl_on_resize,
    gfx_opengl_start_frame,
    gfx_opengl_end_frame,
    gfx_opengl_finish_render,
#ifdef USE_TEXTURE_ATLAS
    gfx_opengl_bind_virtual_texture_page,
    gfx_opengl_create_virtual_texture_page,
    gfx_opengl_upload_virtual_texture,
#endif
    gfx_opengl_signal_start,
};

#endif
