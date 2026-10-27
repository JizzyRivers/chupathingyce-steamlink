/*
GLES2_LAYER.C

The Steam Link's OpenGL ES 2 (a Vivante GC1000) filled out to the ES 3 the
renderer uses (chupathingyce's port/linux/src/d3d8_gl.c with HALO_GL_ES:
HALO_CONSOLE_GLES2, console_gles2_layer). gl_functions_load has loaded what
the driver has; this replaces the entry points ES 2 lacks, or has with less,
in the renderer's own pointers (halo_gl*, gl.h):

- vertex array objects, kept here and applied to ES 2's one set of
  attributes when one is bound
- sampler objects, applied to the textures bound to each unit
- separate read and draw framebuffers, kept here; a blit is a textured quad;
  a depth and stencil attachment is the two of ES 2's packed one
- the sized texture formats, as ES 2's unsized ones, and texture swizzles,
  applied to the texels as they are uploaded (ES 2 has none)
- the shaders: GLSL ES 3.00 source, as the renderer writes it, rewritten as
  GLSL ES 1.00 (attribute and varying, gl_FragColor, texture() as overloads
  of texture2D and textureCube, the vertex shader's attribute locations
  bound before the link)
- what the box has no use for (queries, 3D textures, buffer copies) does
  nothing, saying so once in the log

and the Android host's GL helpers the ES path calls (host_gl_*: xgpu.h).
*/

#include "platform.h"
#include "gl.h"
#include "xgpu.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ES 2's own names for what ES 3 has under its own (GLES2/gl2ext.h) */
#define LAYER_BGRA_EXT 0x80E1
#define LAYER_DEPTH_STENCIL_OES 0x84F9
#define LAYER_UNSIGNED_INT_24_8_OES 0x84FA
#define LAYER_DEPTH24_STENCIL8_OES 0x88F0
#define LAYER_RGBA8_OES 0x8058
/* (desktop GL's: the renderer's overlay sets all four at once) */
#ifndef GL_TEXTURE_SWIZZLE_RGBA
#define GL_TEXTURE_SWIZZLE_RGBA 0x8E46
#endif

#define LAYER_TEXTURE_UNITS 16
#define LAYER_ATTRIBUTES 16
#define LAYER_TRACKED 8192

/* ---------- the driver's own entry points, before they are replaced */

static struct
{
	__typeof__(halo_glGetIntegerv) GetIntegerv;
	__typeof__(halo_glActiveTexture) ActiveTexture;
	__typeof__(halo_glBindTexture) BindTexture;
	__typeof__(halo_glTexParameteri) TexParameteri;
	__typeof__(halo_glTexParameteriv) TexParameteriv;
	__typeof__(halo_glTexParameterf) TexParameterf;
	__typeof__(halo_glTexParameterfv) TexParameterfv;
	__typeof__(halo_glTexImage2D) TexImage2D;
	__typeof__(halo_glTexSubImage2D) TexSubImage2D;
	__typeof__(halo_glCompressedTexImage2D) CompressedTexImage2D;
	__typeof__(halo_glBindFramebuffer) BindFramebuffer;
	__typeof__(halo_glFramebufferTexture2D) FramebufferTexture2D;
	__typeof__(halo_glFramebufferRenderbuffer) FramebufferRenderbuffer;
	__typeof__(halo_glCheckFramebufferStatus) CheckFramebufferStatus;
	__typeof__(halo_glReadPixels) ReadPixels;
	__typeof__(halo_glBindBuffer) BindBuffer;
	__typeof__(halo_glEnableVertexAttribArray) EnableVertexAttribArray;
	__typeof__(halo_glDisableVertexAttribArray) DisableVertexAttribArray;
	__typeof__(halo_glVertexAttribPointer) VertexAttribPointer;
	__typeof__(halo_glCreateShader) CreateShader;
	__typeof__(halo_glShaderSource) ShaderSource;
	__typeof__(halo_glCompileShader) CompileShader;
	__typeof__(halo_glAttachShader) AttachShader;
	__typeof__(halo_glLinkProgram) LinkProgram;
	__typeof__(halo_glUseProgram) UseProgram;
	void (GL_APIENTRY *RenderbufferStorage)(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
	void (GL_APIENTRY *GetFramebufferAttachmentParameteriv)(GLenum target, GLenum attachment, GLenum pname,
		GLint *params);
	void (GL_APIENTRY *VertexAttrib4f)(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
	void (GL_APIENTRY *DiscardFramebufferEXT)(GLenum target, GLsizei count, const GLenum *attachments);
} real;

/* ---------- the log: each kind of thing the layer cannot do, once */

static void layer_once(int *said, const char *format, ...)
{
	char line[256];
	va_list arguments;

	if (*said)
		return;
	*said = 1;
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	platform_log("gles2: %s", line);
}

#define LAYER_ONCE(...) do { static int said; layer_once(&said, __VA_ARGS__); } while (0)

/* HALO_GLES2_TRACE=n: the first n framebuffer, texture, blit and draw calls
in the log, each draw finished (glFinish) and its error read before the
next: the last line before a GPU hang names the call that made it */
static long trace_left = -1;
static int profiling;
static void profile_report(void);

static int tracing(void)
{
	if (trace_left < 0)
	{
		const char *value = getenv("HALO_GLES2_TRACE");

		trace_left = value ? atol(value) : 0;
	}
	return trace_left > 0;
}

static void trace(const char *format, ...)
{
	char line[256];
	va_list arguments;

	if (!tracing())
		return;
	trace_left--;
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	platform_log("gles2 trace: %s", line);
}

/* ---------- textures and samplers */

struct layer_texture
{
	GLsizei width, height;
	/* counts its uploads (a copy of it is made again after one) */
	unsigned long generation;
	/* level 0's format as the driver has it (compressed: the DXT format) */
	GLenum format;
	GLint max_level;
	/* the swizzle the renderer asked for: GL_RED...GL_ALPHA, GL_ZERO, GL_ONE */
	GLint swizzle[4];
	/* the minification filter asked for, and what was last given the driver
	(0: not yet) */
	GLint min_filter;
	GLint applied_min, applied_mag, applied_wrap_s, applied_wrap_t;
};

struct layer_sampler
{
	int used;
	GLint min_filter, mag_filter, wrap_s, wrap_t;
};

static struct layer_texture textures[LAYER_TRACKED];
static struct layer_sampler samplers[LAYER_TRACKED];
static GLuint sampler_next = 1;
static GLuint active_unit;
/* the textures bound to each unit: [0] 2D, [1] cube */
static GLuint unit_textures[LAYER_TEXTURE_UNITS][2];
static GLuint unit_samplers[LAYER_TEXTURE_UNITS];

static struct layer_texture *texture_get(GLuint texture)
{
	return texture && texture < LAYER_TRACKED ? &textures[texture] : NULL;
}

static int target_slot(GLenum target)
{
	return target == GL_TEXTURE_CUBE_MAP ? 1 : 0;
}

/* the texture an image target writes (a cube map's face is the cube's) */
static struct layer_texture *bound_texture(GLenum target)
{
	GLenum binding = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ?
		GL_TEXTURE_CUBE_MAP : target;

	if (active_unit >= LAYER_TEXTURE_UNITS || (binding != GL_TEXTURE_2D && binding != GL_TEXTURE_CUBE_MAP))
		return NULL;
	return texture_get(unit_textures[active_unit][target_slot(binding)]);
}

static void texture_reset(struct layer_texture *texture)
{
	memset(texture, 0, sizeof(*texture));
	texture->max_level = 1000;
	texture->swizzle[0] = GL_RED;
	texture->swizzle[1] = GL_GREEN;
	texture->swizzle[2] = GL_BLUE;
	texture->swizzle[3] = GL_ALPHA;
	texture->min_filter = GL_NEAREST_MIPMAP_LINEAR;
}

/* a mipmapped filter on a texture whose levels stop short of 1x1 leaves it
incomplete in ES 2 (black), where ES 3's GL_TEXTURE_MAX_LEVEL made it whole:
there the filter reads the first level only */
/* the texture's levels stop short of 1x1 (GL_TEXTURE_MAX_LEVEL): ES 3
draws such a texture whole, ES 2 has it incomplete, and Vivante's GPU hangs
sampling one, whatever its filter. It is given level 0 only (texture_level_kept) */
static int short_chain(const struct layer_texture *texture)
{
	GLsizei size = texture->width > texture->height ? texture->width : texture->height;
	GLint levels = 1;

	while (size > 1)
	{
		size >>= 1;
		levels++;
	}
	return texture->max_level + 1 < levels;
}

/* whether an upload of this level reaches the driver */
static int texture_level_kept(const struct layer_texture *texture, GLint level)
{
	if (!level || !texture || !texture->width)
		return 1;
	if (short_chain(texture))
	{
		LAYER_ONCE("textures whose levels stop short of 1x1 keep level 0 only");
		return 0;
	}
	return 1;
}

static GLint complete_min_filter(const struct layer_texture *texture, GLint filter)
{
	static int no_mipmaps = -1;

	/* (HALO_GLES2_NO_MIPMAPS: none sampled, to tell what hangs the GPU) */
	if (no_mipmaps < 0)
		no_mipmaps = getenv("HALO_GLES2_NO_MIPMAPS") != NULL;
	if (filter == GL_NEAREST || filter == GL_LINEAR || (!no_mipmaps && !short_chain(texture)))
		return filter;
	return filter == GL_NEAREST_MIPMAP_NEAREST || filter == GL_NEAREST_MIPMAP_LINEAR ? GL_NEAREST : GL_LINEAR;
}

static int is_texture_parameter_es3(GLenum pname)
{
	switch (pname)
	{
	case GL_TEXTURE_BASE_LEVEL:
	case GL_TEXTURE_MAX_LEVEL:
	case GL_TEXTURE_MIN_LOD:
	case GL_TEXTURE_MAX_LOD:
	case GL_TEXTURE_WRAP_R:
	case GL_TEXTURE_COMPARE_MODE:
	case GL_TEXTURE_COMPARE_FUNC:
	case GL_TEXTURE_SWIZZLE_R:
	case GL_TEXTURE_SWIZZLE_G:
	case GL_TEXTURE_SWIZZLE_B:
	case GL_TEXTURE_SWIZZLE_A:
	case GL_TEXTURE_SWIZZLE_RGBA:
	case GL_TEXTURE_BORDER_COLOR:
	case 0x84FE: /* GL_TEXTURE_MAX_ANISOTROPY_EXT: the box has none */
		return 1;
	default:
		return 0;
	}
}

static GLint wrap_mode(GLint mode)
{
	/* (no border clamp in ES 2) */
	return mode == GL_CLAMP_TO_BORDER ? GL_CLAMP_TO_EDGE : mode;
}

/* the sampler bound to unit, given to the texture bound there (on ES 2 a
texture holds its own filter and wrap) */
static void sampler_apply(GLuint unit)
{
	struct layer_sampler *sampler;
	int slot;

	if (unit >= LAYER_TEXTURE_UNITS || !unit_samplers[unit] || unit_samplers[unit] >= LAYER_TRACKED)
		return;
	sampler = &samplers[unit_samplers[unit]];
	for (slot = 0; slot < 2; slot++)
	{
		GLenum target = slot ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
		struct layer_texture *texture = texture_get(unit_textures[unit][slot]);
		GLint min_filter;

		if (!texture)
			continue;
		min_filter = complete_min_filter(texture, sampler->min_filter);
		if (texture->applied_min == min_filter && texture->applied_mag == sampler->mag_filter &&
			texture->applied_wrap_s == sampler->wrap_s && texture->applied_wrap_t == sampler->wrap_t)
		{
			continue;
		}
		if (unit != active_unit)
			real.ActiveTexture(GL_TEXTURE0 + unit);
		if (texture->applied_min != min_filter)
			real.TexParameteri(target, GL_TEXTURE_MIN_FILTER, min_filter);
		if (texture->applied_mag != sampler->mag_filter)
			real.TexParameteri(target, GL_TEXTURE_MAG_FILTER, sampler->mag_filter);
		if (texture->applied_wrap_s != sampler->wrap_s)
			real.TexParameteri(target, GL_TEXTURE_WRAP_S, sampler->wrap_s);
		if (texture->applied_wrap_t != sampler->wrap_t)
			real.TexParameteri(target, GL_TEXTURE_WRAP_T, sampler->wrap_t);
		if (unit != active_unit)
			real.ActiveTexture(GL_TEXTURE0 + active_unit);
		texture->applied_min = min_filter;
		texture->applied_mag = sampler->mag_filter;
		texture->applied_wrap_s = sampler->wrap_s;
		texture->applied_wrap_t = sampler->wrap_t;
	}
}

static void GL_APIENTRY layer_ActiveTexture(GLenum unit)
{
	active_unit = unit - GL_TEXTURE0;
	real.ActiveTexture(unit);
}

static void GL_APIENTRY layer_BindTexture(GLenum target, GLuint texture)
{
	real.BindTexture(target, texture);
	if (active_unit < LAYER_TEXTURE_UNITS && (target == GL_TEXTURE_2D || target == GL_TEXTURE_CUBE_MAP))
	{
		unit_textures[active_unit][target_slot(target)] = texture;
		sampler_apply(active_unit);
	}
	else if (target != GL_TEXTURE_2D && target != GL_TEXTURE_CUBE_MAP)
	{
		LAYER_ONCE("no texture target 0x%x (3D and array textures are not drawn)", (unsigned)target);
	}
}

static void GL_APIENTRY layer_DeleteTextures_hook(GLsizei count, const GLuint *names);
static __typeof__(halo_glDeleteTextures) real_DeleteTextures;
static __typeof__(halo_glGenTextures) real_GenTextures;

static void GL_APIENTRY layer_GenTextures(GLsizei count, GLuint *names)
{
	GLsizei index;

	real_GenTextures(count, names);
	for (index = 0; index < count; index++)
	{
		struct layer_texture *texture = texture_get(names[index]);

		if (texture)
			texture_reset(texture);
	}
}

static void GL_APIENTRY layer_DeleteTextures_hook(GLsizei count, const GLuint *names)
{
	GLsizei index;
	int unit;

	for (index = 0; index < count; index++)
	{
		for (unit = 0; unit < LAYER_TEXTURE_UNITS; unit++)
		{
			if (unit_textures[unit][0] == names[index])
				unit_textures[unit][0] = 0;
			if (unit_textures[unit][1] == names[index])
				unit_textures[unit][1] = 0;
		}
	}
	real_DeleteTextures(count, names);
}

static void GL_APIENTRY layer_TexParameteri(GLenum target, GLenum pname, GLint value)
{
	struct layer_texture *texture = bound_texture(target);

	switch (pname)
	{
	case GL_TEXTURE_SWIZZLE_R:
	case GL_TEXTURE_SWIZZLE_G:
	case GL_TEXTURE_SWIZZLE_B:
	case GL_TEXTURE_SWIZZLE_A:
		if (texture)
			texture->swizzle[pname - GL_TEXTURE_SWIZZLE_R] = value;
		return;
	case GL_TEXTURE_MAX_LEVEL:
		if (texture)
		{
			texture->max_level = value;
			texture->applied_min = 0;
		}
		return;
	case GL_TEXTURE_MIN_FILTER:
		if (texture)
		{
			texture->min_filter = value;
			value = complete_min_filter(texture, value);
			texture->applied_min = value;
		}
		break;
	case GL_TEXTURE_MAG_FILTER:
		if (texture)
			texture->applied_mag = value;
		break;
	case GL_TEXTURE_WRAP_S:
		value = wrap_mode(value);
		if (texture)
			texture->applied_wrap_s = value;
		break;
	case GL_TEXTURE_WRAP_T:
		value = wrap_mode(value);
		if (texture)
			texture->applied_wrap_t = value;
		break;
	default:
		if (is_texture_parameter_es3(pname))
			return;
		break;
	}
	real.TexParameteri(target, pname, value);
}

static void GL_APIENTRY layer_TexParameteriv(GLenum target, GLenum pname, const GLint *values)
{
	struct layer_texture *texture = bound_texture(target);

	if (pname == GL_TEXTURE_SWIZZLE_RGBA)
	{
		if (texture)
			memcpy(texture->swizzle, values, sizeof(texture->swizzle));
		return;
	}
	layer_TexParameteri(target, pname, values[0]);
}

static void GL_APIENTRY layer_TexParameterf(GLenum target, GLenum pname, GLfloat value)
{
	if (is_texture_parameter_es3(pname))
		return;
	real.TexParameterf(target, pname, value);
}

static void GL_APIENTRY layer_TexParameterfv(GLenum target, GLenum pname, const GLfloat *values)
{
	if (is_texture_parameter_es3(pname))
		return;
	real.TexParameterfv(target, pname, values);
}

static void GL_APIENTRY layer_GenSamplers(GLsizei count, GLuint *names)
{
	GLsizei index;

	for (index = 0; index < count; index++)
	{
		GLuint name = sampler_next < LAYER_TRACKED ? sampler_next++ : 0;

		if (name)
		{
			samplers[name].used = 1;
			samplers[name].min_filter = GL_NEAREST_MIPMAP_LINEAR;
			samplers[name].mag_filter = GL_LINEAR;
			samplers[name].wrap_s = GL_REPEAT;
			samplers[name].wrap_t = GL_REPEAT;
		}
		else
		{
			LAYER_ONCE("out of sampler objects");
		}
		names[index] = name;
	}
}

static void GL_APIENTRY layer_BindSampler(GLuint unit, GLuint sampler)
{
	if (unit >= LAYER_TEXTURE_UNITS)
		return;
	unit_samplers[unit] = sampler < LAYER_TRACKED ? sampler : 0;
	sampler_apply(unit);
}

static void sampler_parameter(GLuint name, GLenum pname, GLint value)
{
	struct layer_sampler *sampler;
	GLuint unit;

	if (!name || name >= LAYER_TRACKED)
		return;
	sampler = &samplers[name];
	switch (pname)
	{
	case GL_TEXTURE_MIN_FILTER: sampler->min_filter = value; break;
	case GL_TEXTURE_MAG_FILTER: sampler->mag_filter = value; break;
	case GL_TEXTURE_WRAP_S: sampler->wrap_s = wrap_mode(value); break;
	case GL_TEXTURE_WRAP_T: sampler->wrap_t = wrap_mode(value); break;
	default: return;
	}
	for (unit = 0; unit < LAYER_TEXTURE_UNITS; unit++)
	{
		if (unit_samplers[unit] == name)
			sampler_apply(unit);
	}
}

static void GL_APIENTRY layer_SamplerParameteri(GLuint sampler, GLenum pname, GLint value)
{
	sampler_parameter(sampler, pname, value);
}

static void GL_APIENTRY layer_SamplerParameterf(GLuint sampler, GLenum pname, GLfloat value)
{
	sampler_parameter(sampler, pname, (GLint)value);
}

static void GL_APIENTRY layer_SamplerParameterfv(GLuint sampler, GLenum pname, const GLfloat *values)
{
	sampler_parameter(sampler, pname, (GLint)values[0]);
}

/* ---------- texture images */

static int swizzle_identity(const GLint swizzle[4])
{
	return swizzle[0] == GL_RED && swizzle[1] == GL_GREEN && swizzle[2] == GL_BLUE && swizzle[3] == GL_ALPHA;
}

/* RGBA texels with the texture's swizzle applied, in a buffer the caller
frees; NULL where none is needed */
static unsigned char *swizzled_texels(const struct layer_texture *texture, GLenum format, GLenum type,
	GLsizei width, GLsizei height, const void *pixels)
{
	const unsigned char *source = pixels;
	unsigned char *result, *destination;
	size_t count, index;
	int channel;

	if (!texture || !pixels || swizzle_identity(texture->swizzle))
		return NULL;
	if (format != GL_RGBA || type != GL_UNSIGNED_BYTE)
	{
		LAYER_ONCE("no swizzle for format 0x%x type 0x%x", (unsigned)format, (unsigned)type);
		return NULL;
	}
	count = (size_t)width * (size_t)height;
	result = malloc(count * 4);
	if (!result)
		return NULL;
	destination = result;
	for (index = 0; index < count; index++, source += 4, destination += 4)
	{
		for (channel = 0; channel < 4; channel++)
		{
			switch (texture->swizzle[channel])
			{
			case GL_RED: destination[channel] = source[0]; break;
			case GL_GREEN: destination[channel] = source[1]; break;
			case GL_BLUE: destination[channel] = source[2]; break;
			case GL_ALPHA: destination[channel] = source[3]; break;
			case GL_ZERO: destination[channel] = 0; break;
			default: destination[channel] = 255; break;
			}
		}
	}
	return result;
}

/* ES 3's sized internal format as ES 2's format and type (ES 2's internal
format is its format) */
static int image_format(GLint *internal, GLenum *format, GLenum *type)
{
	switch (*internal)
	{
	case GL_RGBA8:
	case GL_RGBA:
		*internal = GL_RGBA;
		*format = GL_RGBA;
		return 1;
	case GL_RGB8:
	case GL_RGB:
		*internal = GL_RGB;
		*format = GL_RGB;
		return 1;
	case GL_RGB565:
		*internal = GL_RGB;
		*format = GL_RGB;
		*type = GL_UNSIGNED_SHORT_5_6_5;
		return 1;
	case GL_RGBA4:
	case GL_RGB5_A1:
		*internal = GL_RGBA;
		*format = GL_RGBA;
		return 1;
	case GL_R8:
	case GL_RED:
		/* (sampled as red, green and blue all three: the overlay reads red) */
		*internal = GL_LUMINANCE;
		*format = GL_LUMINANCE;
		return 1;
	case GL_RG8:
	case GL_RG:
		*internal = GL_LUMINANCE_ALPHA;
		*format = GL_LUMINANCE_ALPHA;
		return 1;
	case GL_DEPTH24_STENCIL8:
	case GL_DEPTH_STENCIL:
		*internal = LAYER_DEPTH_STENCIL_OES;
		*format = LAYER_DEPTH_STENCIL_OES;
		*type = LAYER_UNSIGNED_INT_24_8_OES;
		return 1;
	case GL_DEPTH_COMPONENT24:
	case GL_DEPTH_COMPONENT32F:
	case GL_DEPTH_COMPONENT16:
	case GL_DEPTH_COMPONENT:
		*internal = GL_DEPTH_COMPONENT;
		*format = GL_DEPTH_COMPONENT;
		*type = GL_UNSIGNED_INT;
		return 1;
	default:
		LAYER_ONCE("no internal format 0x%x", (unsigned)*internal);
		return 0;
	}
}

static void GL_APIENTRY layer_TexImage2D(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height,
	GLint border, GLenum format, GLenum type, const void *pixels)
{
	struct layer_texture *texture = bound_texture(target);
	unsigned char *swizzled;

	if (!image_format(&internal, &format, &type))
		return;
	if (!texture_level_kept(texture, level))
		return;
	if (texture && level == 0)
	{
		texture->width = width;
		texture->height = height;
		texture->format = (GLenum)internal;
		texture->applied_min = 0;
	}
	swizzled = swizzled_texels(texture, format, type, width, height, pixels);
	if (texture)
		texture->generation++;
	if (level == 0)
		trace("teximage target 0x%x %dx%d internal 0x%x format 0x%x type 0x%x%s", (unsigned)target, (int)width,
			(int)height, (unsigned)internal, (unsigned)format, (unsigned)type, pixels ? "" : " (empty)");
	real.TexImage2D(target, level, internal, width, height, border, format, type, swizzled ? swizzled : pixels);
	free(swizzled);
}

static void GL_APIENTRY layer_TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width,
	GLsizei height, GLenum format, GLenum type, const void *pixels)
{
	struct layer_texture *texture = bound_texture(target);
	unsigned char *swizzled;

	if (!texture_level_kept(texture, level))
		return;
	if (texture)
		texture->generation++;
	if (format == GL_RED)
		format = GL_LUMINANCE;
	else if (format == GL_RG)
		format = GL_LUMINANCE_ALPHA;
	swizzled = swizzled_texels(texture, format, type, width, height, pixels);
	real.TexSubImage2D(target, level, x, y, width, height, format, type, swizzled ? swizzled : pixels);
	free(swizzled);
}

static void GL_APIENTRY layer_CompressedTexImage2D(GLenum target, GLint level, GLenum internal, GLsizei width,
	GLsizei height, GLint border, GLsizei size, const void *data)
{
	struct layer_texture *texture = bound_texture(target);

	if (!texture_level_kept(texture, level))
		return;
	if (texture && level == 0)
	{
		texture->width = width;
		texture->height = height;
		texture->format = internal;
		texture->applied_min = 0;
	}
	if (texture)
		texture->generation++;
	if (texture && !swizzle_identity(texture->swizzle))
		LAYER_ONCE("a compressed texture's swizzle is not applied");
	real.CompressedTexImage2D(target, level, internal, width, height, border, size, data);
}

static void GL_APIENTRY layer_TexImage3D(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height,
	GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels)
{
	(void)target; (void)level; (void)internal; (void)width; (void)height; (void)depth; (void)border;
	(void)format; (void)type; (void)pixels;
	LAYER_ONCE("no 3D or array textures");
}

static void GL_APIENTRY layer_CompressedTexImage3D(GLenum target, GLint level, GLenum internal, GLsizei width,
	GLsizei height, GLsizei depth, GLint border, GLsizei size, const void *data)
{
	(void)target; (void)level; (void)internal; (void)width; (void)height; (void)depth; (void)border;
	(void)size; (void)data;
	LAYER_ONCE("no 3D or array textures");
}

/* ---------- buffers, in the process's memory

The renderer streams its vertices and indices into a ring of large buffers
(16 MB and 2 MB, d3d8_gl.c), a few hundred writes a frame; Vivante's ES 2
flushes the CPU's caches over the buffer for each, which made a frame take
seconds. Here a buffer object is memory of the process's, and a draw hands
the driver pointers into it (ES 2's client arrays), which copies only what
the draw reads. No buffer object reaches the driver. */

struct layer_buffer
{
	unsigned char *data;
	/* -1 for a name made with nothing in it yet */
	GLsizeiptr size;
};

static struct layer_buffer buffers[LAYER_TRACKED];
static GLuint buffer_next = 1;
/* GL_COPY_READ_BUFFER's and GL_COPY_WRITE_BUFFER's (the mirror's copies) */
static GLuint copy_read_buffer, copy_write_buffer;

static struct layer_buffer *buffer_get(GLuint name)
{
	return name && name < LAYER_TRACKED ? &buffers[name] : NULL;
}

/* where an offset into a buffer is in memory (a client pointer, with no
buffer) */
static const void *buffer_pointer(GLuint name, const void *offset)
{
	struct layer_buffer *buffer = buffer_get(name);

	if (!buffer)
		return offset;
	return buffer->data ? buffer->data + (uintptr_t)offset : NULL;
}

static void GL_APIENTRY layer_GenBuffers(GLsizei count, GLuint *names)
{
	GLsizei index;

	for (index = 0; index < count; index++)
	{
		while (buffer_next < LAYER_TRACKED && buffers[buffer_next].size != 0)
			buffer_next++;
		if (buffer_next >= LAYER_TRACKED)
		{
			LAYER_ONCE("out of buffer objects");
			names[index] = 0;
			continue;
		}
		buffers[buffer_next].size = -1;
		names[index] = buffer_next++;
	}
}

static void GL_APIENTRY layer_DeleteBuffers(GLsizei count, const GLuint *names)
{
	GLsizei index;

	for (index = 0; index < count; index++)
	{
		struct layer_buffer *buffer = buffer_get(names[index]);

		if (!buffer)
			continue;
		free(buffer->data);
		buffer->data = NULL;
		buffer->size = 0;
		if (names[index] < buffer_next)
			buffer_next = names[index];
	}
}

static GLuint *buffer_binding(GLenum target);

static void GL_APIENTRY layer_BufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
	GLuint *binding = buffer_binding(target);
	struct layer_buffer *buffer = binding ? buffer_get(*binding) : NULL;
	unsigned char *memory;

	(void)usage;
	if (!buffer)
		return;
	/* (the same size again is the renderer orphaning it: kept, as the
	driver has copied what the draws before it read) */
	if (buffer->data && buffer->size == size)
	{
		if (data)
			memcpy(buffer->data, data, (size_t)size);
		return;
	}
	/* (malloc's untouched pages cost nothing: a 16 MB ring buffer is as much
	of the box's memory as is streamed into it) */
	memory = size > 0 ? malloc((size_t)size) : NULL;
	if (size > 0 && !memory)
	{
		LAYER_ONCE("no memory for a buffer of %ld bytes", (long)size);
		return;
	}
	free(buffer->data);
	buffer->data = memory;
	buffer->size = size;
	if (data && memory)
		memcpy(memory, data, (size_t)size);
}

static void GL_APIENTRY layer_BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
	GLuint *binding = buffer_binding(target);
	struct layer_buffer *buffer = binding ? buffer_get(*binding) : NULL;

	if (!buffer || !buffer->data || offset < 0 || size < 0 || offset + size > buffer->size)
		return;
	memcpy(buffer->data + offset, data, (size_t)size);
}

/* ---------- vertex arrays */

struct layer_attribute
{
	GLuint buffer;
	GLint size;
	GLenum type;
	GLboolean normalized;
	GLsizei stride;
	const void *pointer;
};

struct layer_vertex_array
{
	int used;
	unsigned int enabled;
	GLuint element_buffer;
	struct layer_attribute attributes[LAYER_ATTRIBUTES];
};

/* [0] is the default one */
static struct layer_vertex_array vertex_arrays[256];
static GLuint vertex_array_current;
static GLuint array_buffer;
static GLint attribute_maximum;

static struct layer_vertex_array *vertex_array(void)
{
	return &vertex_arrays[vertex_array_current];
}

/* the attribute as a client array, in its buffer's memory */
static void attribute_apply(GLuint index, const struct layer_attribute *attribute)
{
	real.VertexAttribPointer(index, attribute->size ? attribute->size : 4,
		attribute->type ? attribute->type : GL_FLOAT, attribute->normalized, attribute->stride,
		buffer_pointer(attribute->buffer, attribute->pointer));
}

static void GL_APIENTRY layer_GenVertexArrays(GLsizei count, GLuint *names)
{
	GLsizei index;
	GLuint name;

	for (index = 0; index < count; index++)
	{
		for (name = 1; name < sizeof(vertex_arrays) / sizeof(*vertex_arrays) && vertex_arrays[name].used; name++)
		{
		}
		if (name >= sizeof(vertex_arrays) / sizeof(*vertex_arrays))
		{
			LAYER_ONCE("out of vertex arrays");
			names[index] = 0;
			continue;
		}
		memset(&vertex_arrays[name], 0, sizeof(vertex_arrays[name]));
		vertex_arrays[name].used = 1;
		names[index] = name;
	}
}

static void GL_APIENTRY layer_BindVertexArray(GLuint name)
{
	struct layer_vertex_array *from = vertex_array(), *to;
	GLuint index;

	if (name >= sizeof(vertex_arrays) / sizeof(*vertex_arrays))
		name = 0;
	if (name == vertex_array_current)
		return;
	to = &vertex_arrays[name];
	vertex_array_current = name;
	for (index = 0; index < (GLuint)attribute_maximum && index < LAYER_ATTRIBUTES; index++)
	{
		unsigned int bit = 1u << index;

		if (to->enabled & bit)
		{
			attribute_apply(index, &to->attributes[index]);
			if (!(from->enabled & bit))
				real.EnableVertexAttribArray(index);
		}
		else if (from->enabled & bit)
		{
			real.DisableVertexAttribArray(index);
		}
	}
}

/* the binding a target names (the element array buffer is the vertex
array's) */
static GLuint *buffer_binding(GLenum target)
{
	switch (target)
	{
	case GL_ARRAY_BUFFER: return &array_buffer;
	case GL_ELEMENT_ARRAY_BUFFER: return &vertex_array()->element_buffer;
	case GL_COPY_READ_BUFFER: return &copy_read_buffer;
	case GL_COPY_WRITE_BUFFER: return &copy_write_buffer;
	default:
		/* (uniform, atomic counter buffers: none in ES 2) */
		LAYER_ONCE("no buffer target 0x%x", (unsigned)target);
		return NULL;
	}
}

static void GL_APIENTRY layer_BindBuffer(GLenum target, GLuint buffer)
{
	GLuint *binding = buffer_binding(target);

	if (binding)
		*binding = buffer;
}

static void GL_APIENTRY layer_EnableVertexAttribArray(GLuint index)
{
	if (index >= (GLuint)attribute_maximum)
	{
		LAYER_ONCE("vertex attribute %u past the box's %d", index, attribute_maximum);
		return;
	}
	vertex_array()->enabled |= 1u << index;
	real.EnableVertexAttribArray(index);
}

static void GL_APIENTRY layer_DisableVertexAttribArray(GLuint index)
{
	if (index >= (GLuint)attribute_maximum)
		return;
	vertex_array()->enabled &= ~(1u << index);
	real.DisableVertexAttribArray(index);
}

static void GL_APIENTRY layer_VertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
	GLsizei stride, const void *pointer)
{
	struct layer_attribute *attribute;

	if (index >= (GLuint)attribute_maximum || index >= LAYER_ATTRIBUTES)
		return;
	attribute = &vertex_array()->attributes[index];
	attribute->buffer = array_buffer;
	attribute->size = size;
	attribute->type = type;
	attribute->normalized = normalized;
	attribute->stride = stride;
	attribute->pointer = pointer;
	attribute_apply(index, attribute);
}

static void GL_APIENTRY layer_VertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride,
	const void *pointer)
{
	/* (the packed normals, D3DVSDT_NORMPACKED3: no integer attributes in ES 2) */
	LAYER_ONCE("integer vertex attributes read as floats (packed normals are wrong)");
	layer_VertexAttribPointer(index, size, type == GL_UNSIGNED_INT || type == GL_INT ? GL_UNSIGNED_SHORT : type,
		GL_FALSE, stride, pointer);
}

static void GL_APIENTRY layer_VertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w)
{
	if (index < (GLuint)attribute_maximum)
		real.VertexAttrib4f(index, (GLfloat)x, (GLfloat)y, (GLfloat)z, (GLfloat)w);
}

static GLuint program_current;
static GLuint read_framebuffer, draw_framebuffer;
/* each framebuffer's color texture (feedback_begin) */
static GLuint framebuffer_colors[LAYER_TRACKED];
static __typeof__(halo_glDrawArrays) real_DrawArrays;
static __typeof__(halo_glDrawElements) real_DrawElements;
static GLuint draw_framebuffer_seen(void);

/* A draw that samples the texture it draws into: the Xbox let a shader
read the render target (the menus' and the screen effects' passes), as do
desktop GL and Mali, but Vivante's GPU hangs on it until the kernel driver
resets it 20 seconds later. Those units sample a copy of the framebuffer
instead, made just before the draw. */
static GLuint feedback_texture;
static GLsizei feedback_width, feedback_height;
static void (GL_APIENTRY *real_CopyTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x,
	GLint y, GLsizei width, GLsizei height);

/* returns the units switched to the copy (a bit each), to put back after */
static unsigned int feedback_begin(void)
{
	GLuint color = draw_framebuffer < LAYER_TRACKED ? framebuffer_colors[draw_framebuffer] : 0;
	struct layer_texture *texture;
	unsigned int units = 0, unit;

	if (!draw_framebuffer || !color)
		return 0;
	for (unit = 0; unit < LAYER_TEXTURE_UNITS; unit++)
	{
		if (unit_textures[unit][0] == color)
			units |= 1u << unit;
	}
	if (!units)
		return 0;
	texture = texture_get(color);
	if (!texture || !texture->width || !texture->height || !real_CopyTexSubImage2D)
		return 0;
	LAYER_ONCE("a draw samples its own render target (texture %u): it samples a copy", color);
	real.ActiveTexture(GL_TEXTURE0);
	if (!feedback_texture)
		real_GenTextures(1, &feedback_texture);
	real.BindTexture(GL_TEXTURE_2D, feedback_texture);
	if (feedback_width != texture->width || feedback_height != texture->height)
	{
		real.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture->width, texture->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
			NULL);
		real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		feedback_width = texture->width;
		feedback_height = texture->height;
	}
	real_CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, texture->width, texture->height);
	for (unit = 0; unit < LAYER_TEXTURE_UNITS; unit++)
	{
		if (units & (1u << unit))
		{
			real.ActiveTexture(GL_TEXTURE0 + unit);
			real.BindTexture(GL_TEXTURE_2D, feedback_texture);
		}
	}
	real.ActiveTexture(GL_TEXTURE0 + active_unit);
	return units;
}

static void feedback_end(unsigned int units)
{
	unsigned int unit;

	if (!units)
		return;
	for (unit = 0; unit < LAYER_TEXTURE_UNITS; unit++)
	{
		if (units & (1u << unit))
		{
			real.ActiveTexture(GL_TEXTURE0 + unit);
			real.BindTexture(GL_TEXTURE_2D, unit_textures[unit][0]);
		}
	}
	real.ActiveTexture(GL_TEXTURE0 + active_unit);
}

/* One texture sampled from two units (the menus' widgets: their bitmap on
stages 0 and 1), with a mipmapped texture on another: Vivante's GPU hangs on
the draw until the kernel driver resets it 20 seconds later, whether the two
samplers read two units or one. With a different texture on the later unit
it does not, so that unit samples a copy: the texture drawn once into an
RGBA texture of its size (one level), kept until the texture changes. */
struct shared_copy
{
	GLuint texture, framebuffer;
	GLsizei width, height;
	/* the texture's uploads when the copy was made (layer_texture.generation) */
	unsigned long generation;
};

static struct shared_copy shared_copies[LAYER_TRACKED];

static int blit_program_make(void);
static GLint blit_texture_uniform, blit_rectangle_uniform;
static GLuint blit_program;

/* the copy of a texture, made (or made again) now: the state it changes is
put back as the draw it is made for had it */
static GLuint shared_copy_get(GLuint name)
{
	static const GLfloat corners[] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
	static const GLfloat whole[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
	static const GLenum capabilities[] = { GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_CULL_FACE, GL_SCISSOR_TEST,
		GL_DITHER };
	struct layer_texture *texture = texture_get(name);
	struct shared_copy *copy;
	struct layer_vertex_array *current = vertex_array();
	GLboolean enabled[sizeof(capabilities) / sizeof(*capabilities)], mask[4];
	GLint viewport[4];
	GLuint index;

	if (!texture || !texture->width || !texture->height || name >= LAYER_TRACKED || !blit_program_make())
		return 0;
	copy = &shared_copies[name];
	if (copy->texture && copy->width == texture->width && copy->height == texture->height &&
		copy->generation == texture->generation)
	{
		return copy->texture;
	}

	if (!copy->texture)
	{
		real_GenTextures(1, &copy->texture);
		halo_glGenFramebuffers(1, &copy->framebuffer);
	}
	real.ActiveTexture(GL_TEXTURE0);
	real.BindTexture(GL_TEXTURE_2D, copy->texture);
	real.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture->width, texture->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	real.BindFramebuffer(GL_FRAMEBUFFER, copy->framebuffer);
	real.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, copy->texture, 0);

	/* the state the draw had */
	for (index = 0; index < sizeof(capabilities) / sizeof(*capabilities); index++)
	{
		enabled[index] = halo_glIsEnabled(capabilities[index]);
		halo_glDisable(capabilities[index]);
	}
	real.GetIntegerv(GL_VIEWPORT, viewport);
	{
		GLint values[4];

		real.GetIntegerv(GL_COLOR_WRITEMASK, values);
		for (index = 0; index < 4; index++)
			mask[index] = (GLboolean)values[index];
	}
	halo_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	halo_glViewport(0, 0, texture->width, texture->height);
	for (index = 1; index < (GLuint)attribute_maximum; index++)
	{
		if (current->enabled & (1u << index))
			real.DisableVertexAttribArray(index);
	}

	/* the copy, drawn from the texture as it is (its own filter: level 0
	of a mipmapped one) */
	real.UseProgram(blit_program);
	real.BindTexture(GL_TEXTURE_2D, name);
	real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	texture->applied_min = texture->applied_mag = 0;
	halo_glUniform1i(blit_texture_uniform, 0);
	halo_glUniform4fv(blit_rectangle_uniform, 1, whole);
	real.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, corners);
	if (!(current->enabled & 1))
		real.EnableVertexAttribArray(0);
	real_DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	/* back as the draw had it */
	if (current->enabled & 1)
		attribute_apply(0, &current->attributes[0]);
	else
		real.DisableVertexAttribArray(0);
	for (index = 1; index < (GLuint)attribute_maximum; index++)
	{
		if (current->enabled & (1u << index))
			real.EnableVertexAttribArray(index);
	}
	real.UseProgram(program_current);
	real.BindFramebuffer(GL_FRAMEBUFFER, draw_framebuffer);
	halo_glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	halo_glColorMask(mask[0], mask[1], mask[2], mask[3]);
	for (index = 0; index < sizeof(capabilities) / sizeof(*capabilities); index++)
	{
		if (enabled[index])
			halo_glEnable(capabilities[index]);
	}
	real.BindTexture(GL_TEXTURE_2D, unit_textures[0][0]);
	/* (the texture's filter, as its sampler has it, at its next binding) */
	sampler_apply(0);
	real.ActiveTexture(GL_TEXTURE0 + active_unit);

	copy->width = texture->width;
	copy->height = texture->height;
	copy->generation = texture->generation;
	LAYER_ONCE("a texture sampled from two units: the later samples a copy");
	return copy->texture;
}

/* the later units that share a texture with an earlier one, given its copy
for the draw; returns them, to put back (shared_units_end) */
static unsigned int shared_units_begin(void)
{
	unsigned int moved = 0, unit, earlier;

	for (unit = 1; unit < 4; unit++)
	{
		GLuint copy;

		if (!unit_textures[unit][0])
			continue;
		for (earlier = 0; earlier < unit; earlier++)
		{
			if (unit_textures[earlier][0] == unit_textures[unit][0])
				break;
		}
		if (earlier == unit)
			continue;
		copy = shared_copy_get(unit_textures[unit][0]);
		if (!copy)
			continue;
		real.ActiveTexture(GL_TEXTURE0 + unit);
		real.BindTexture(GL_TEXTURE_2D, copy);
		real.ActiveTexture(GL_TEXTURE0 + active_unit);
		moved |= 1u << unit;
	}
	return moved;
}

static void shared_units_end(unsigned int moved)
{
	unsigned int unit;

	for (unit = 1; unit < 4; unit++)
	{
		if (moved & (1u << unit))
		{
			real.ActiveTexture(GL_TEXTURE0 + unit);
			real.BindTexture(GL_TEXTURE_2D, unit_textures[unit][0]);
			real.ActiveTexture(GL_TEXTURE0 + active_unit);
		}
	}
}

/* A small draw whose vertices all have the same position (attribute 0,
the Xbox's v0): no pixel of it is drawn, but with textures bound
Vivante's GPU can hang on it until the kernel driver resets it 20 seconds
later (the menus draw one every frame). Left out. */
static int draw_degenerate(GLint first_vertex, GLsizei count, GLenum type, const void *indices)
{
	const struct layer_vertex_array *current = vertex_array();
	const struct layer_attribute *position = &current->attributes[0];
	const unsigned char *base;
	size_t size, stride;
	GLsizei number;

	if (!(current->enabled & 1) || count <= 0 || count > 64 || position->type != GL_FLOAT)
		return 0;
	base = buffer_pointer(position->buffer, position->pointer);
	if (!base)
		return 0;
	size = (size_t)(position->size ? position->size : 4) * sizeof(float);
	stride = position->stride ? (size_t)position->stride : size;
	for (number = 1; number < count; number++)
	{
		size_t first = (size_t)first_vertex, vertex = (size_t)first_vertex + (size_t)number;

		if (indices)
		{
			first = type == GL_UNSIGNED_INT ? ((const GLuint *)indices)[0] : type == GL_UNSIGNED_SHORT ?
				((const GLushort *)indices)[0] : ((const GLubyte *)indices)[0];
			vertex = type == GL_UNSIGNED_INT ? ((const GLuint *)indices)[number] : type == GL_UNSIGNED_SHORT ?
				((const GLushort *)indices)[number] : ((const GLubyte *)indices)[number];
		}
		if (memcmp(base + first * stride, base + vertex * stride, size))
			return 0;
	}
	LAYER_ONCE("draws whose vertices are all in one place are left out (the GPU can hang on them)");
	return 1;
}

/* HALO_GLES2_EXPERIMENT=program:bits, for one program's draws while
hunting a GPU hang: 1, its textures a white 1x1; 2, only the attributes the
program reads enabled; 4, not drawn at all. experiment_begin returns what to
put back (experiment_end). */
static GLuint experiment_program, experiment_white;
static unsigned int experiment_bits;

static unsigned int experiment_begin(void)
{
	static int read;
	unsigned int undo = 0, unit;

	if (!read)
	{
		const char *value = getenv("HALO_GLES2_EXPERIMENT");

		read = 1;
		if (value && strchr(value, ':'))
		{
			experiment_program = (GLuint)atoi(value);
			experiment_bits = (unsigned int)atoi(strchr(value, ':') + 1);
			platform_log("gles2: experiment on program %u: %u", experiment_program, experiment_bits);
		}
	}
	if (!experiment_bits || program_current != experiment_program)
		return 0;
	if (experiment_bits & (1 | 8 | 16 | 32))
	{
		static const unsigned char white[4] = { 255, 255, 255, 255 };

		if (!experiment_white)
		{
			real_GenTextures(1, &experiment_white);
			real.ActiveTexture(GL_TEXTURE0);
			real.BindTexture(GL_TEXTURE_2D, experiment_white);
			real.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
			real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		}
		/* (1: units 0 and 1, 8: units 2 and 3) */
		for (unit = 0; unit < 4; unit++)
		{
			/* (16: unit 1 alone, 32: unit 0 alone) */
			if (!(experiment_bits & (unit < 2 ? 1 : 8)) && !(unit == 1 && (experiment_bits & 16)) &&
				!(unit == 0 && (experiment_bits & 32)))
			{
				continue;
			}
			real.ActiveTexture(GL_TEXTURE0 + unit);
			real.BindTexture(GL_TEXTURE_2D, experiment_white);
		}
		real.ActiveTexture(GL_TEXTURE0 + active_unit);
		undo |= 1;
	}
	if (experiment_bits & 2)
	{
		GLint count = 0, index;
		unsigned int used = 0;
		struct layer_vertex_array *current = vertex_array();

		static void (GL_APIENTRY *get_active)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *);
		static GLint (GL_APIENTRY *get_location)(GLuint, const GLchar *);

		if (!get_active)
		{
			get_active = (__typeof__(get_active))SDL_GL_GetProcAddress("glGetActiveAttrib");
			get_location = (__typeof__(get_location))SDL_GL_GetProcAddress("glGetAttribLocation");
		}
		halo_glGetProgramiv(program_current, GL_ACTIVE_ATTRIBUTES, &count);
		for (index = 0; index < count; index++)
		{
			char name[64];
			GLint size;
			GLenum type;
			GLint location;

			get_active(program_current, (GLuint)index, sizeof(name), NULL, &size, &type, name);
			location = get_location(program_current, name);
			if (location >= 0)
				used |= 1u << location;
		}
		for (unit = 0; unit < (unsigned int)attribute_maximum; unit++)
		{
			if ((current->enabled & (1u << unit)) && !(used & (1u << unit)))
				real.DisableVertexAttribArray(unit);
		}
		undo |= 2;
	}
	if (experiment_bits & 4)
		undo |= 4;
	return undo;
}

static void experiment_end(unsigned int undo)
{
	unsigned int unit;

	if (undo & 1)
	{
		for (unit = 0; unit < 4; unit++)
		{
			real.ActiveTexture(GL_TEXTURE0 + unit);
			real.BindTexture(GL_TEXTURE_2D, unit_textures[unit][0]);
		}
		real.ActiveTexture(GL_TEXTURE0 + active_unit);
	}
	if (undo & 2)
	{
		struct layer_vertex_array *current = vertex_array();

		for (unit = 0; unit < (unsigned int)attribute_maximum; unit++)
		{
			if (current->enabled & (1u << unit))
				real.EnableVertexAttribArray(unit);
		}
	}
}

/* HALO_GLES2_SLOW: every draw finished (glFinish) and timed, and one that
took the GPU past a second in the log with what it drew with: Vivante's
kernel driver resets a hung GPU after 20 seconds, so a draw that hangs it
shows as a frame of 20 */
static int slow_checking = -1;

static void program_dump(GLuint program, const char *tag);

static void draw_slow_check(const char *kind, GLenum mode, GLsizei count, GLenum type, Uint64 start)
{
	static int reported;
	Uint64 took;
	struct layer_vertex_array *current;
	char attributes[160];
	size_t length = 0;
	GLuint index;

	halo_glFinish();
	took = SDL_GetTicks() - start;
	if (took < 1000 || reported >= 20)
		return;
	reported++;
	current = vertex_array();
	attributes[0] = 0;
	for (index = 0; index < (GLuint)attribute_maximum && length + 24 < sizeof(attributes); index++)
	{
		const struct layer_attribute *attribute = &current->attributes[index];

		if (current->enabled & (1u << index))
			length += (size_t)snprintf(attributes + length, sizeof(attributes) - length, " %u:%dx%04x/%d", index,
				(int)attribute->size, (unsigned)attribute->type, (int)attribute->stride);
	}
	platform_log("gles2: a %s took %llu ms: mode %u count %d type 0x%x program %u framebuffer %u, "
		"textures %u %u %u %u, attributes%s", kind, (unsigned long long)took, (unsigned)mode, (int)count,
		(unsigned)type, program_current, draw_framebuffer_seen(), unit_textures[0][0], unit_textures[1][0],
		unit_textures[2][0], unit_textures[3][0], attributes);
	for (index = 0; index < 4; index++)
	{
		struct layer_texture *texture = texture_get(unit_textures[index][0]);

		if (texture)
			platform_log("gles2:   unit %u: texture %u, %dx%d, format 0x%x, levels to %d, min filter 0x%x (given 0x%x), "
				"swizzle %x %x %x %x", index, unit_textures[index][0], (int)texture->width, (int)texture->height,
				(unsigned)texture->format, (int)texture->max_level, (unsigned)texture->applied_min,
				(unsigned)texture->min_filter, (unsigned)texture->swizzle[0], (unsigned)texture->swizzle[1],
				(unsigned)texture->swizzle[2], (unsigned)texture->swizzle[3]);
	}
	program_dump(program_current, "slow");
	/* its vertices, as the attributes the program reads give them */
	for (index = 0; index < (GLuint)attribute_maximum; index++)
	{
		const struct layer_attribute *attribute = &current->attributes[index];
		const float *vertex = buffer_pointer(attribute->buffer, attribute->pointer);
		int number;

		if (!(current->enabled & (1u << index)) || !vertex || attribute->type != GL_FLOAT || mode != GL_TRIANGLE_FAN)
			continue;
		for (number = 0; number < count && number < 4; number++)
		{
			const float *value = (const float *)((const unsigned char *)vertex + number * attribute->stride);

			platform_log("gles2:   attribute %u vertex %d: %g %g %g %g", index, number, value[0], value[1], value[2],
				value[3]);
		}
	}
	{
		static void (GL_APIENTRY *get_uniform)(GLuint, GLint, GLfloat *);
		static const char *const names[] = { "texture_scale[0]", "texture_scale[1]", "texture_scale[2]",
			"texture_lod_bias", "viewport_scale", "viewport_offset" };
		GLuint name;

		if (!get_uniform)
			get_uniform = (__typeof__(get_uniform))SDL_GL_GetProcAddress("glGetUniformfv");
		for (name = 0; name < sizeof(names) / sizeof(*names); name++)
		{
			GLint location = halo_glGetUniformLocation(program_current, names[name]);
			GLfloat value[4] = { 0 };

			if (location < 0 || !get_uniform)
				continue;
			get_uniform(program_current, location, value);
			platform_log("gles2:   %s: %g %g %g %g", names[name], value[0], value[1], value[2], value[3]);
		}
	}
}

static void draw_traced(const char *kind, GLenum mode, GLsizei count)
{
	GLenum error;

	if (!tracing())
		return;
	halo_glFinish();
	error = halo_glGetError();
	trace("%s mode %u count %d program %u framebuffer %u: done, error 0x%x", kind, (unsigned)mode, (int)count,
		program_current, draw_framebuffer_seen(), (unsigned)error);
}

static void GL_APIENTRY layer_DrawArrays(GLenum mode, GLint first, GLsizei count)
{
	Uint64 start = 0;

	if (slow_checking < 0)
		slow_checking = getenv("HALO_GLES2_SLOW") != NULL;
	if (slow_checking)
	{
		halo_glFinish();
		start = SDL_GetTicks();
	}
	if (profiling)
		profile_report();
	if (tracing())
		trace("draw arrays mode %u count %d program %u", (unsigned)mode, (int)count, program_current);
	{
		unsigned int feedback = feedback_begin();
		unsigned int experiment = experiment_begin();

		if (!(experiment & 4) && !draw_degenerate(first, count, 0, NULL))
		{
			unsigned int shared = shared_units_begin();

			real_DrawArrays(mode, first, count);
			shared_units_end(shared);
		}
		experiment_end(experiment);
		feedback_end(feedback);
	}
	if (slow_checking)
		draw_slow_check("draw arrays", mode, count, 0, start);
	draw_traced("draw arrays", mode, count);
}

static void GL_APIENTRY layer_DrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	Uint64 start = 0;

	if (slow_checking < 0)
		slow_checking = getenv("HALO_GLES2_SLOW") != NULL;
	if (slow_checking)
	{
		halo_glFinish();
		start = SDL_GetTicks();
	}
	if (profiling)
		profile_report();
	if (tracing())
		trace("draw elements mode %u count %d type 0x%x program %u", (unsigned)mode, (int)count, (unsigned)type,
			program_current);
	indices = buffer_pointer(vertex_array()->element_buffer, indices);
	if (indices)
	{
		unsigned int feedback = feedback_begin();
		unsigned int experiment = experiment_begin();

		if (!(experiment & 4) && !draw_degenerate(0, count, type, indices))
		{
			unsigned int shared = shared_units_begin();

			real_DrawElements(mode, count, type, indices);
			shared_units_end(shared);
		}
		experiment_end(experiment);
		feedback_end(feedback);
	}
	if (slow_checking)
		draw_slow_check("draw elements", mode, count, type, start);
	draw_traced("draw elements", mode, count);
}

static void GL_APIENTRY layer_DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices,
	GLint base_vertex)
{
	(void)base_vertex;
	LAYER_ONCE("no base vertex");
	layer_DrawElements(mode, count, type, indices);
}

/* ---------- framebuffers */

static GLuint draw_framebuffer_seen(void)
{
	return draw_framebuffer;
}

/* the framebuffer a target names, bound for the call (ES 2 has one) */
static GLuint framebuffer_for(GLenum target)
{
	GLuint framebuffer = target == GL_READ_FRAMEBUFFER ? read_framebuffer : draw_framebuffer;

	if (framebuffer != draw_framebuffer)
		real.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	return framebuffer;
}

static void framebuffer_done(GLuint framebuffer)
{
	if (framebuffer != draw_framebuffer)
		real.BindFramebuffer(GL_FRAMEBUFFER, draw_framebuffer);
}

static void GL_APIENTRY layer_BindFramebuffer(GLenum target, GLuint framebuffer)
{
	if (target == GL_READ_FRAMEBUFFER)
	{
		read_framebuffer = framebuffer;
		return;
	}
	if (target == GL_FRAMEBUFFER)
		read_framebuffer = framebuffer;
	draw_framebuffer = framebuffer;
	real.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
}

static void GL_APIENTRY layer_FramebufferTexture2D(GLenum target, GLenum attachment, GLenum texture_target,
	GLuint texture, GLint level)
{
	GLuint framebuffer = framebuffer_for(target);

	trace("framebuffer %u attachment 0x%x texture %u", framebuffer, (unsigned)attachment, texture);
	if (attachment == GL_COLOR_ATTACHMENT0 && framebuffer < LAYER_TRACKED)
		framebuffer_colors[framebuffer] = texture;
	if (attachment == GL_DEPTH_STENCIL_ATTACHMENT)
	{
		real.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, texture_target, texture, level);
		real.FramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, texture_target, texture, level);
	}
	else
	{
		real.FramebufferTexture2D(GL_FRAMEBUFFER, attachment, texture_target, texture, level);
	}
	framebuffer_done(framebuffer);
}

static void GL_APIENTRY layer_FramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffer_target,
	GLuint renderbuffer)
{
	GLuint framebuffer = framebuffer_for(target);

	if (attachment == GL_DEPTH_STENCIL_ATTACHMENT)
	{
		real.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, renderbuffer_target, renderbuffer);
		real.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, renderbuffer_target, renderbuffer);
	}
	else
	{
		real.FramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, renderbuffer_target, renderbuffer);
	}
	framebuffer_done(framebuffer);
}

static GLenum GL_APIENTRY layer_CheckFramebufferStatus(GLenum target)
{
	GLuint framebuffer = framebuffer_for(target);
	GLenum status = real.CheckFramebufferStatus(GL_FRAMEBUFFER);

	trace("framebuffer %u status 0x%x", framebuffer, (unsigned)status);

	framebuffer_done(framebuffer);
	return status;
}

static void GL_APIENTRY layer_ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
	GLenum type, void *pixels)
{
	GLuint framebuffer = framebuffer_for(GL_READ_FRAMEBUFFER);

	real.ReadPixels(x, y, width, height, format, type, pixels);
	framebuffer_done(framebuffer);
}

static void GL_APIENTRY layer_DrawBuffers(GLsizei count, const GLenum *buffers)
{
	(void)count;
	(void)buffers;
}

static void GL_APIENTRY layer_ReadBuffer(GLenum buffer)
{
	(void)buffer;
}

static void GL_APIENTRY layer_InvalidateFramebuffer(GLenum target, GLsizei count, const GLenum *attachments)
{
	(void)target;
	if (real.DiscardFramebufferEXT && draw_framebuffer)
		real.DiscardFramebufferEXT(GL_FRAMEBUFFER, count, attachments);
}

static void GL_APIENTRY layer_RenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internal,
	GLsizei width, GLsizei height)
{
	if (samples > 1)
		LAYER_ONCE("no multisampling");
	if (internal == GL_DEPTH24_STENCIL8)
		internal = LAYER_DEPTH24_STENCIL8_OES;
	else if (internal == GL_RGBA8)
		internal = LAYER_RGBA8_OES;
	real.RenderbufferStorage(target, internal, width, height);
}

/* HALO_GLES2_SHOT=folder: the window's picture as a PPM there, every 300th
frame presented (shot0.ppm, shot1.ppm...): what the box shows, over ssh */
static void screenshot_maybe(GLint x, GLint y, GLsizei width, GLsizei height)
{
	static unsigned long frames, shots;
	const char *folder = getenv("HALO_GLES2_SHOT");
	unsigned char *pixels;
	char path[512];
	FILE *file;
	GLsizei row;

	if (!folder || width <= 0 || height <= 0 || ++frames % 300)
		return;
	pixels = malloc((size_t)width * (size_t)height * 4);
	if (!pixels)
		return;
	real.ReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	snprintf(path, sizeof(path), "%s/shot%lu.ppm", folder, shots++ % 8);
	file = fopen(path, "wb");
	if (file)
	{
		fprintf(file, "P6\n%d %d\n255\n", (int)width, (int)height);
		/* (bottom row first in GL: the file's top row first) */
		for (row = height - 1; row >= 0; row--)
		{
			const unsigned char *texel = pixels + (size_t)row * (size_t)width * 4;
			GLsizei column;

			for (column = 0; column < width; column++, texel += 4)
				fwrite(texel, 1, 3, file);
		}
		fclose(file);
		platform_log("gles2: frame %lu saved to %s", frames, path);
	}
	free(pixels);
}

/* a blit: the read framebuffer's color texture drawn as a quad into the
draw framebuffer */
static int blit_program_make(void)
{
	static const char *vertex =
		"attribute vec2 corner;\n"
		"uniform vec4 source;\n"
		"varying vec2 coordinates;\n"
		"void main()\n"
		"{\n"
		"	coordinates = source.xy + corner * source.zw;\n"
		"	gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	static const char *fragment =
		"precision mediump float;\n"
		"uniform sampler2D picture;\n"
		"varying vec2 coordinates;\n"
		"void main()\n"
		"{\n"
		"	gl_FragColor = texture2D(picture, coordinates);\n"
		"}\n";
	GLuint shaders[2], index;
	GLint linked = 0;

	if (blit_program)
		return 1;
	for (index = 0; index < 2; index++)
	{
		const char *source = index ? fragment : vertex;

		shaders[index] = real.CreateShader(index ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
		real.ShaderSource(shaders[index], 1, &source, NULL);
		real.CompileShader(shaders[index]);
	}
	blit_program = halo_glCreateProgram();
	real.AttachShader(blit_program, shaders[0]);
	real.AttachShader(blit_program, shaders[1]);
	halo_glBindAttribLocation(blit_program, 0, "corner");
	real.LinkProgram(blit_program);
	halo_glGetProgramiv(blit_program, GL_LINK_STATUS, &linked);
	if (!linked)
	{
		LAYER_ONCE("the blit's program does not link");
		blit_program = 0;
		return 0;
	}
	blit_texture_uniform = halo_glGetUniformLocation(blit_program, "picture");
	blit_rectangle_uniform = halo_glGetUniformLocation(blit_program, "source");
	return 1;
}

static void GL_APIENTRY layer_BlitFramebuffer(GLint source_x0, GLint source_y0, GLint source_x1, GLint source_y1,
	GLint destination_x0, GLint destination_y0, GLint destination_x1, GLint destination_y1, GLbitfield mask,
	GLenum filter)
{
	static const GLfloat corners[] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
	GLint type = 0, name = 0;
	struct layer_texture *texture;
	struct layer_vertex_array *current = vertex_array();
	GLuint framebuffer;

	if (!(mask & GL_COLOR_BUFFER_BIT))
	{
		LAYER_ONCE("no depth or stencil blits");
		return;
	}
	framebuffer = framebuffer_for(GL_READ_FRAMEBUFFER);
	if (read_framebuffer)
	{
		real.GetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
		real.GetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
	}
	framebuffer_done(framebuffer);
	texture = texture_get((GLuint)name);
	/* a blit into the framebuffer it reads (the renderer's first present
	makes its back buffer's framebuffer with GL_FRAMEBUFFER, which leaves it
	the draw framebuffer too): ES 3 copies over itself, here it would draw
	from the texture it draws into, which the GPU does not survive */
	if (read_framebuffer == draw_framebuffer)
	{
		LAYER_ONCE("a blit into the framebuffer it reads (%u): left out", read_framebuffer);
		return;
	}
	if (type != GL_TEXTURE || !texture || !texture->width || !texture->height)
	{
		LAYER_ONCE("a blit from something not a texture (framebuffer %u)", read_framebuffer);
		return;
	}
	if (!blit_program_make())
		return;

	/* everything the renderer has set is set again after (xgpu.h) */
	halo_glDisable(GL_BLEND);
	halo_glDisable(GL_DEPTH_TEST);
	halo_glDisable(GL_STENCIL_TEST);
	halo_glDisable(GL_CULL_FACE);
	halo_glDisable(GL_SCISSOR_TEST);
	halo_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	halo_glViewport(destination_x0 < destination_x1 ? destination_x0 : destination_x1,
		destination_y0 < destination_y1 ? destination_y0 : destination_y1,
		abs(destination_x1 - destination_x0), abs(destination_y1 - destination_y0));
	real.UseProgram(blit_program);
	real.ActiveTexture(GL_TEXTURE0);
	real.BindTexture(GL_TEXTURE_2D, (GLuint)name);
	real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	texture->applied_min = texture->applied_mag = 0;
	halo_glUniform1i(blit_texture_uniform, 0);
	{
		/* (a flipped rectangle, either way, flips the picture) */
		GLfloat source[4];

		source[0] = (GLfloat)source_x0 / (GLfloat)texture->width;
		source[1] = (GLfloat)source_y0 / (GLfloat)texture->height;
		source[2] = (GLfloat)(source_x1 - source_x0) / (GLfloat)texture->width;
		source[3] = (GLfloat)(source_y1 - source_y0) / (GLfloat)texture->height;
		if (destination_x1 < destination_x0)
		{
			source[0] += source[2];
			source[2] = -source[2];
		}
		if (destination_y1 < destination_y0)
		{
			source[1] += source[3];
			source[3] = -source[3];
		}
		halo_glUniform4fv(blit_rectangle_uniform, 1, source);
	}
	/* only attribute 0: the GPU reads every enabled one for each vertex,
	wherever its buffer and offset point now */
	{
		GLuint index;

		for (index = 1; index < (GLuint)attribute_maximum; index++)
		{
			if (current->enabled & (1u << index))
				real.DisableVertexAttribArray(index);
		}
	}
	real.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, corners);
	if (!(current->enabled & 1))
		real.EnableVertexAttribArray(0);
	trace("blit %d,%d-%d,%d of framebuffer %u (texture %d) to %d,%d-%d,%d of %u", source_x0, source_y0, source_x1,
		source_y1, read_framebuffer, name, destination_x0, destination_y0, destination_x1, destination_y1,
		draw_framebuffer);
	real_DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	/* the vertex array's attribute 0, the unit's texture and the buffer
	back as they were; the rest the renderer sets again */
	if (current->enabled & 1)
		attribute_apply(0, &current->attributes[0]);
	else
		real.DisableVertexAttribArray(0);
	{
		GLuint index;

		for (index = 1; index < (GLuint)attribute_maximum; index++)
		{
			if (current->enabled & (1u << index))
				real.EnableVertexAttribArray(index);
		}
	}
	real.BindTexture(GL_TEXTURE_2D, unit_textures[0][0]);
	if (active_unit != 0)
		real.ActiveTexture(GL_TEXTURE0 + active_unit);
	xgpu_gl_state_invalidate();
	if (!draw_framebuffer)
	{
		/* the first frames' times in the log, presented to presented */
		static Uint64 last;
		static int timed;
		Uint64 now = SDL_GetTicks();

		if (profiling)
			profile_report();
		if (timed++ < 30)
			platform_log("gles2: frame %d presented, %llu ms after the last", timed, (unsigned long long)(now - last));
		last = now;
		screenshot_maybe(destination_x0 < destination_x1 ? destination_x0 : destination_x1,
			destination_y0 < destination_y1 ? destination_y0 : destination_y1, abs(destination_x1 - destination_x0),
			abs(destination_y1 - destination_y0));
	}
}

/* ---------- queries, buffer copies */

static void GL_APIENTRY layer_GenQueries(GLsizei count, GLuint *names)
{
	GLsizei index;

	for (index = 0; index < count; index++)
		names[index] = (GLuint)index + 1;
}

static void GL_APIENTRY layer_BeginQuery(GLenum target, GLuint name)
{
	(void)target;
	(void)name;
}

static void GL_APIENTRY layer_EndQuery(GLenum target)
{
	(void)target;
}

static void GL_APIENTRY layer_GetQueryObjectuiv(GLuint name, GLenum pname, GLuint *value)
{
	(void)name;
	/* available, and everything visible (lens flares at full strength) */
	*value = pname == GL_QUERY_RESULT_AVAILABLE ? 1 : 1000000;
}

static void GL_APIENTRY layer_CopyBufferSubData(GLenum read_target, GLenum write_target, GLintptr read_offset,
	GLintptr write_offset, GLsizeiptr size)
{
	GLuint *read_binding = buffer_binding(read_target), *write_binding = buffer_binding(write_target);
	struct layer_buffer *from = read_binding ? buffer_get(*read_binding) : NULL;
	struct layer_buffer *to = write_binding ? buffer_get(*write_binding) : NULL;

	if (!from || !to || !from->data || !to->data || read_offset < 0 || write_offset < 0 || size < 0 ||
		read_offset + size > from->size || write_offset + size > to->size)
	{
		return;
	}
	memmove(to->data + write_offset, from->data + read_offset, (size_t)size);
}

static void GL_APIENTRY layer_BindBufferBase(GLenum target, GLuint index, GLuint buffer)
{
	(void)target; (void)index; (void)buffer;
}

static void GL_APIENTRY layer_BindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset,
	GLsizeiptr size)
{
	(void)target; (void)index; (void)buffer; (void)offset; (void)size;
}

/* ---------- state queries */

static void GL_APIENTRY layer_GetIntegerv(GLenum name, GLint *value)
{
	switch (name)
	{
	/* ES 3.0 is what the renderer is told it has (gl_initialize) */
	case GL_MAJOR_VERSION: *value = 3; return;
	case GL_MINOR_VERSION: *value = 0; return;
	case GL_VERTEX_ARRAY_BINDING: *value = (GLint)vertex_array_current; return;
	case GL_SAMPLER_BINDING: *value = active_unit < LAYER_TEXTURE_UNITS ? (GLint)unit_samplers[active_unit] : 0; return;
	case GL_DRAW_FRAMEBUFFER_BINDING: *value = (GLint)draw_framebuffer; return;
	case GL_READ_FRAMEBUFFER_BINDING: *value = (GLint)read_framebuffer; return;
	case GL_MAX_SAMPLES: *value = 0; return;
	case GL_TEXTURE_BINDING_2D_ARRAY:
	case GL_TEXTURE_BINDING_3D:
	case GL_NUM_EXTENSIONS:
		*value = 0;
		return;
	default:
		real.GetIntegerv(name, value);
		return;
	}
}

/* ---------- shaders: GLSL ES 3.00 as GLSL ES 1.00 */

struct layer_shader
{
	GLenum type;
	char *source;
	/* a vertex shader's attributes and their locations (layout(location)) */
	int attribute_count;
	char attribute_names[LAYER_ATTRIBUTES][32];
	GLint attribute_locations[LAYER_ATTRIBUTES];
};

static struct layer_shader shaders[LAYER_TRACKED];
/* the vertex shader each program has */
static GLuint program_vertex_shaders[LAYER_TRACKED];
static GLuint program_fragment_shaders[LAYER_TRACKED];

struct text
{
	char *buffer;
	size_t length, size;
};

static void text_append(struct text *text, const char *data, size_t length)
{
	if (text->length + length + 1 > text->size)
	{
		size_t size = text->size ? text->size : 4096;
		char *buffer;

		while (text->length + length + 1 > size)
			size *= 2;
		buffer = realloc(text->buffer, size);
		if (!buffer)
			return;
		text->buffer = buffer;
		text->size = size;
	}
	memcpy(text->buffer + text->length, data, length);
	text->length += length;
	text->buffer[text->length] = 0;
}

static void text_add(struct text *text, const char *string)
{
	text_append(text, string, strlen(string));
}

static int starts_with(const char *text, const char *prefix)
{
	return !strncmp(text, prefix, strlen(prefix));
}

static int is_name_character(char character)
{
	return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
		(character >= '0' && character <= '9') || character == '_';
}

/* source with every whole-word from replaced by to */
static void replace_words(struct text *text, const char *source, size_t length, const char *const *pairs)
{
	size_t index = 0;

	while (index < length)
	{
		const char *const *pair;
		int replaced = 0;

		if (!index || !is_name_character(source[index - 1]))
		{
			for (pair = pairs; *pair; pair += 2)
			{
				size_t word = strlen(pair[0]);

				if (index + word <= length && !strncmp(source + index, pair[0], word) &&
					(index + word == length || !is_name_character(source[index + word])))
				{
					text_add(text, pair[1]);
					index += word;
					replaced = 1;
					break;
				}
			}
		}
		if (!replaced)
		{
			text_append(text, source + index, 1);
			index++;
		}
	}
}

/* "TYPE NAME;" (after the qualifier): NAME */
static void declaration_name(const char *declaration, char *name, size_t size)
{
	const char *end = strchr(declaration, ';'), *start;
	size_t length;

	name[0] = 0;
	if (!end)
		return;
	while (end > declaration && (end[-1] == ' ' || end[-1] == ']' || end[-1] == '[' ||
		(end[-1] >= '0' && end[-1] <= '9' && strchr(declaration, '['))))
	{
		if (end[-1] == '[')
		{
			end--;
			break;
		}
		end--;
	}
	start = end;
	while (start > declaration && is_name_character(start[-1]))
		start--;
	length = (size_t)(end - start);
	if (length >= size)
		length = size - 1;
	memcpy(name, start, length);
	name[length] = 0;
}

/* (texture() with a bias samples without it: Vivante's GPU hangs on some
draws that sample with one, and the renderer's biases are 0 unless a
texture asks) */
static const char fragment_helpers[] =
	"vec4 texture(sampler2D s, vec2 c) { return texture2D(s, c); }\n"
	"vec4 texture(sampler2D s, vec2 c, float b) { return texture2D(s, c); }\n"
	"vec4 texture(sampler2D s, vec3 c) { return texture2D(s, c.xy); }\n"
	"vec4 texture(sampler2D s, vec3 c, float b) { return texture2D(s, c.xy); }\n"
	"vec4 texture(samplerCube s, vec3 c) { return textureCube(s, c); }\n"
	"vec4 texture(samplerCube s, vec3 c, float b) { return textureCube(s, c); }\n"
	"vec4 textureLod(sampler2D s, vec2 c, float l) { return texture2D(s, c); }\n"
	"vec4 textureLod(samplerCube s, vec3 c, float l) { return textureCube(s, c); }\n";
static const char vertex_helpers[] =
	"vec4 texture(sampler2D s, vec2 c) { return texture2DLod(s, c, 0.0); }\n"
	"vec4 texture(samplerCube s, vec3 c) { return textureCubeLod(s, c, 0.0); }\n"
	"vec4 textureLod(sampler2D s, vec2 c, float l) { return texture2DLod(s, c, l); }\n";
static const char common_helpers[] =
	"float round(float x) { return floor(x + 0.5); }\n"
	"vec2 round(vec2 x) { return floor(x + 0.5); }\n"
	"vec3 round(vec3 x) { return floor(x + 0.5); }\n"
	"vec4 round(vec4 x) { return floor(x + 0.5); }\n"
	"float trunc(float x) { return x < 0.0 ? -floor(-x) : floor(x); }\n"
	"int clamp_int(int x, int low, int high) { return x < low ? low : (x > high ? high : x); }\n";

/* text the generators write that ES 1.00 has under another name, or not at
all (its built-in functions cannot be overloaded: clamp is float's only) */
static const char *const text_replacements[] = {
	/* the vertex shader's relative constants, c[clamp(a0 + n, 0, 191)] */
	"[clamp(a0", "[clamp_int(a0",
	NULL,
};

/* source with each of pairs' text replaced wherever it is */
static void replace_text(struct text *text, const char *source, const char *const *pairs)
{
	while (*source)
	{
		const char *const *pair;
		int replaced = 0;

		for (pair = pairs; *pair; pair += 2)
		{
			size_t length = strlen(pair[0]);

			if (!strncmp(source, pair[0], length))
			{
				text_add(text, pair[1]);
				source += length;
				replaced = 1;
				break;
			}
		}
		if (!replaced)
			text_append(text, source++, 1);
	}
}

/* the whole-word appearances of word in text */
static int word_count(const char *text, const char *word)
{
	size_t length = strlen(word);
	const char *at = text;
	int count = 0;

	while ((at = strstr(at, word)) != NULL)
	{
		if ((at == text || !is_name_character(at[-1])) && !is_name_character(at[length]))
			count++;
		at += length;
	}
	return count;
}

/* The vertex shader declares the Xbox's 16 registers (vN_in, or vN_packed)
and copies each into vN; the box has 10 attributes, and its compiler counts
a copy nobody reads. A register whose vN is read nowhere else is no
attribute here: its copy takes the register's default instead. */
static void attributes_prune(struct layer_shader *shader, struct text *source)
{
	int reg;

	for (reg = 0; reg < 16; reg++)
	{
		static const char *const suffixes[] = { "_in", "_packed" };
		char local[8], attribute[16], declaration[48];
		const char *at;
		struct text pruned = { 0 };
		const char *pairs[5];
		size_t kind;
		int index;

		snprintf(local, sizeof(local), "v%d", reg);
		if (word_count(source->buffer, local) > 1)
			continue;
		for (kind = 0; kind < 2; kind++)
		{
			snprintf(attribute, sizeof(attribute), "v%d%s", reg, suffixes[kind]);
			if (!word_count(source->buffer, attribute))
				continue;
			snprintf(declaration, sizeof(declaration), "attribute %s %s;\n", kind ? "float" : "vec4", attribute);
			at = strstr(source->buffer, declaration);
			if (!at)
				continue;
			/* (the declaration, then the name as a whole word: v5_in is not
			v15_in) */
			pairs[0] = declaration;
			pairs[1] = "";
			pairs[2] = NULL;
			replace_text(&pruned, source->buffer, pairs);
			free(source->buffer);
			*source = pruned;
			memset(&pruned, 0, sizeof(pruned));
			pairs[0] = attribute;
			pairs[1] = kind ? "0.0" : "vec4(0.0, 0.0, 0.0, 1.0)";
			replace_words(&pruned, source->buffer, source->length, pairs);
			free(source->buffer);
			*source = pruned;
			for (index = 0; index < shader->attribute_count; index++)
			{
				if (!strcmp(shader->attribute_names[index], attribute))
				{
					shader->attribute_locations[index] = -1;
					break;
				}
			}
			break;
		}
	}
}

static char *shader_translate(struct layer_shader *shader, const char *source)
{
	static const char *const words[] = {
		"sampler3D", "sampler2D",
		"gl_FragDepth", "gl_FragDepthEXT",
		/* (the packed normals' attribute: no integers in ES 1.00's
		attributes, nor bit shifts to unpack them with) */
		"uint", "float",
		NULL,
	};
	struct text body = { 0 }, result = { 0 };
	const char *line = source;
	int fragment = shader->type == GL_FRAGMENT_SHADER;
	int skipping = 0;

	shader->attribute_count = 0;
	while (*line)
	{
		const char *end = strchr(line, '\n');
		size_t length = end ? (size_t)(end - line) : strlen(line);
		const char *text = line;
		char declaration[512];
		char name[64];

		while (*text == ' ' || *text == '\t')
			text++;
		if (skipping)
		{
			/* to the function's closing brace */
			if (*text == '}')
				skipping = 0;
		}
		else if (starts_with(text, "vec4 unpack_normpacked3("))
		{
			/* D3DVSDT_NORMPACKED3 unpacked with bit shifts: a normal straight
			up in ES 1.00 (the models' lighting is wrong; the menus have none) */
			text_add(&body, "vec4 unpack_normpacked3(float p) { return vec4(0.0, 0.0, 1.0, 1.0); }\n");
			skipping = !memchr(text, '}', length - (size_t)(text - line));
		}
		else if (length >= sizeof(declaration))
		{
			replace_words(&body, line, length, words);
			text_add(&body, "\n");
		}
		else if (starts_with(text, "#version") || (starts_with(text, "precision") &&
			(strstr(text, "sampler3D") || strstr(text, "sampler2DArray") || strstr(text, "Shadow"))) ||
			strstr(text, "atomic_uint") || strstr(text, "early_fragment_tests"))
		{
			/* (ES 1.00 has none of them; the #version comes first, below) */
		}
		else
		{
			int location = -1;

			memcpy(declaration, text, length - (size_t)(text - line));
			declaration[length - (size_t)(text - line)] = 0;
			text = declaration;
			if (starts_with(text, "layout("))
			{
				const char *close = strchr(text, ')');
				const char *at = strstr(text, "location");

				if (at && close && at < close)
				{
					at = strchr(at, '=');
					if (at)
						location = atoi(at + 1);
				}
				text = close ? close + 1 : text;
				while (*text == ' ')
					text++;
			}
			while (starts_with(text, "flat ") || starts_with(text, "smooth ") || starts_with(text, "centroid ") ||
				starts_with(text, "noperspective ") || starts_with(text, "highp ") || starts_with(text, "mediump "))
			{
				text = strchr(text, ' ') + 1;
			}
			if (starts_with(text, "in ") && strchr(text, ';') && !strchr(text, '('))
			{
				text_add(&body, fragment ? "varying " : "attribute ");
				replace_words(&body, text + 3, strlen(text + 3), words);
				text_add(&body, "\n");
				if (!fragment && shader->attribute_count < LAYER_ATTRIBUTES && location >= 0)
				{
					declaration_name(text + 3, name, sizeof(name));
					strncpy(shader->attribute_names[shader->attribute_count], name,
						sizeof(shader->attribute_names[0]) - 1);
					shader->attribute_locations[shader->attribute_count++] = location;
				}
			}
			else if (starts_with(text, "out ") && strchr(text, ';') && !strchr(text, '('))
			{
				if (fragment)
				{
					declaration_name(text + 4, name, sizeof(name));
					text_add(&body, "#define ");
					text_add(&body, name);
					text_add(&body, " gl_FragColor\n");
				}
				else
				{
					text_add(&body, "varying ");
					replace_words(&body, text + 4, strlen(text + 4), words);
					text_add(&body, "\n");
				}
			}
			else
			{
				replace_words(&body, line, length, words);
				text_add(&body, "\n");
			}
		}
		line = end ? end + 1 : line + length;
	}

	text_add(&result, "#version 100\n");
	if (fragment)
	{
		if (body.buffer && (strstr(body.buffer, "dFdx") || strstr(body.buffer, "dFdy") || strstr(body.buffer, "fwidth")))
			text_add(&result, "#extension GL_OES_standard_derivatives : enable\n");
		if (body.buffer && strstr(body.buffer, "gl_FragDepthEXT"))
			text_add(&result, "#extension GL_EXT_frag_depth : enable\n");
	}
	text_add(&result, "precision highp float;\nprecision highp int;\n");
	text_add(&result, common_helpers);
	text_add(&result, fragment ? fragment_helpers : vertex_helpers);
	if (body.buffer)
		replace_text(&result, body.buffer, text_replacements);
	free(body.buffer);
	if (!fragment && result.buffer)
		attributes_prune(shader, &result);
	return result.buffer;
}

static GLuint GL_APIENTRY layer_CreateShader(GLenum type)
{
	GLuint shader = real.CreateShader(type);

	if (shader && shader < LAYER_TRACKED)
	{
		free(shaders[shader].source);
		memset(&shaders[shader], 0, sizeof(shaders[shader]));
		shaders[shader].type = type;
	}
	return shader;
}

static void GL_APIENTRY layer_ShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings,
	const GLint *lengths)
{
	struct text source = { 0 };
	char *translated;
	GLsizei index;

	for (index = 0; index < count; index++)
	{
		if (lengths && lengths[index] >= 0)
			text_append(&source, strings[index], (size_t)lengths[index]);
		else
			text_add(&source, strings[index]);
	}
	if (!source.buffer || !shader || shader >= LAYER_TRACKED)
	{
		real.ShaderSource(shader, count, strings, lengths);
		free(source.buffer);
		return;
	}
	translated = shader_translate(&shaders[shader], source.buffer);
	free(source.buffer);
	free(shaders[shader].source);
	shaders[shader].source = translated;
	if (translated)
	{
		const GLchar *text = translated;

		real.ShaderSource(shader, 1, &text, NULL);
	}
}

/* a shader that does not compile: its error and its translated source in
the log (the first few) */
static void GL_APIENTRY layer_CompileShader(GLuint shader)
{
	static int reported;
	GLint compiled = 0;

	real.CompileShader(shader);
	halo_glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (!compiled && reported < 6 && shader < LAYER_TRACKED && shaders[shader].source)
	{
		char log[1024] = "";
		const char *line = shaders[shader].source;
		int number = 1;

		reported++;
		halo_glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		platform_log("gles2: a %s shader does not compile: %s", shaders[shader].type == GL_VERTEX_SHADER ? "vertex" :
			"fragment", log);
		while (*line)
		{
			const char *end = strchr(line, '\n');
			int length = end ? (int)(end - line) : (int)strlen(line);

			platform_log("gles2: %4d %.*s", number++, length, line);
			line += length + (end ? 1 : 0);
		}
	}
}

static void GL_APIENTRY layer_AttachShader(GLuint program, GLuint shader)
{
	if (program < LAYER_TRACKED && shader < LAYER_TRACKED && shaders[shader].type == GL_VERTEX_SHADER)
		program_vertex_shaders[program] = shader;
	else if (program < LAYER_TRACKED && shader < LAYER_TRACKED && shaders[shader].type == GL_FRAGMENT_SHADER)
		program_fragment_shaders[program] = shader;
	real.AttachShader(program, shader);
}

/* a program's two shaders, as given the driver, in HALO_GLES2_DUMP */
static void program_dump(GLuint program, const char *tag)
{
	const char *folder = getenv("HALO_GLES2_DUMP");
	GLuint pair[2];
	int stage;

	if (!folder || program >= LAYER_TRACKED)
		return;
	pair[0] = program_vertex_shaders[program];
	pair[1] = program_fragment_shaders[program];
	for (stage = 0; stage < 2; stage++)
	{
		char path[512];
		FILE *file;

		if (!pair[stage] || !shaders[pair[stage]].source)
			continue;
		snprintf(path, sizeof(path), "%s/%s%u.%s", folder, tag, program, stage ? "frag" : "vert");
		file = fopen(path, "w");
		if (file)
		{
			fputs(shaders[pair[stage]].source, file);
			fclose(file);
		}
	}
}

static void GL_APIENTRY layer_LinkProgram(GLuint program)
{
	static int reported;
	GLint linked = 0;

	if (program < LAYER_TRACKED && program_vertex_shaders[program])
	{
		struct layer_shader *vertex = &shaders[program_vertex_shaders[program]];
		int index;

		for (index = 0; index < vertex->attribute_count; index++)
		{
			if (vertex->attribute_locations[index] < 0)
				continue;
			if (vertex->attribute_locations[index] < attribute_maximum)
				halo_glBindAttribLocation(program, (GLuint)vertex->attribute_locations[index],
					vertex->attribute_names[index]);
			else
				LAYER_ONCE("attribute %s at %d, past the box's %d", vertex->attribute_names[index],
					vertex->attribute_locations[index], attribute_maximum);
		}
	}
	real.LinkProgram(program);
	halo_glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked && reported < 6)
	{
		char log[1024] = "";

		reported++;
		halo_glGetProgramInfoLog(program, sizeof(log), NULL, log);
		/* (HALO_GLES2_DUMP: the pair that does not link, in that folder) */
		if (getenv("HALO_GLES2_DUMP") && program < LAYER_TRACKED)
		{
			GLuint pair[2] = { program_vertex_shaders[program], program_fragment_shaders[program] };
			int stage;

			for (stage = 0; stage < 2; stage++)
			{
				char path[512];
				FILE *file;

				if (!pair[stage] || !shaders[pair[stage]].source)
					continue;
				snprintf(path, sizeof(path), "%s/link%d.%s", getenv("HALO_GLES2_DUMP"), reported,
					stage ? "frag" : "vert");
				file = fopen(path, "w");
				if (file)
				{
					fputs(shaders[pair[stage]].source, file);
					fclose(file);
				}
			}
		}
		platform_log("gles2: a program does not link: %s", log);
	}
}

static void GL_APIENTRY layer_UseProgram(GLuint program)
{
	program_current = program;
	real.UseProgram(program);
}

/* ---------- the Android host's GL helpers (xgpu.h) */

int host_gl_has_extension(const char *name)
{
	const char *extensions = (const char *)halo_glGetString(GL_EXTENSIONS);
	size_t length = strlen(name);
	const char *at = extensions;

	while (at && (at = strstr(at, name)) != NULL)
	{
		if ((at == extensions || at[-1] == ' ') && (at[length] == ' ' || at[length] == 0))
			return 1;
		at += length;
	}
	return 0;
}

void host_gl_read_buffer(unsigned int buffer, unsigned int offset, unsigned int size, void *data)
{
	(void)buffer;
	(void)offset;
	/* (ES 2 cannot read a buffer back) */
	memset(data, 0, size);
}

void host_gl_buffer_write(unsigned int target, unsigned int offset, unsigned int size, const void *data)
{
	halo_glBufferSubData(target, (GLintptr)offset, (GLsizeiptr)size, data);
}

/* (no fences in ES 2: the driver orders a buffer's writes after the draws
that read it) */
void host_gl_fence_frame(unsigned int slot)
{
	(void)slot;
}

void host_gl_wait_frame(unsigned int slot)
{
	(void)slot;
}

/* ---------- a sampling profiler (HALO_GLES2_PROFILE)

The box has no perf and no gdb: a SIGPROF 100 times a second of the
process's time records the program counter it interrupted (and the link
register, for code in a library), and every 10 seconds the most common go
to the log, for llvm-symbolizer against the unstripped executable. */

#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#include <ucontext.h>

#define PROFILE_SLOTS 4096

static volatile unsigned int profile_pcs[PROFILE_SLOTS], profile_counts[PROFILE_SLOTS];
static volatile unsigned int profile_total;

static void profile_record(unsigned int pc)
{
	unsigned int slot = (pc >> 2) % PROFILE_SLOTS, probe;

	for (probe = 0; probe < 16; probe++, slot = (slot + 1) % PROFILE_SLOTS)
	{
		if (profile_pcs[slot] == pc || !profile_pcs[slot])
		{
			profile_pcs[slot] = pc;
			profile_counts[slot]++;
			return;
		}
	}
}

static void profile_signal(int signal_number, siginfo_t *information, void *context)
{
	ucontext_t *ucontext = context;

	(void)signal_number;
	(void)information;
	profile_record((unsigned int)ucontext->uc_mcontext.arm_pc);
	/* (the caller too, as the pc | 1: a library's own pc says little) */
	profile_record((unsigned int)ucontext->uc_mcontext.arm_lr | 1u);
	profile_total++;
}

static void profile_report(void)
{
	static Uint64 last;
	Uint64 now = SDL_GetTicks();
	unsigned int best[12], best_count[12], slot, index, total = profile_total;

	if (now - last < 9000)
		return;
	last = now;
	memset(best_count, 0, sizeof(best_count));
	memset(best, 0, sizeof(best));
	for (slot = 0; slot < PROFILE_SLOTS; slot++)
	{
		unsigned int count = profile_counts[slot];

		for (index = 0; index < 12; index++)
		{
			if (count > best_count[index])
			{
				memmove(&best[index + 1], &best[index], (11 - index) * sizeof(*best));
				memmove(&best_count[index + 1], &best_count[index], (11 - index) * sizeof(*best_count));
				best[index] = profile_pcs[slot];
				best_count[index] = count;
				break;
			}
		}
	}
	platform_log("gles2 profile: %u samples", total);
	for (index = 0; index < 12 && best_count[index]; index++)
		platform_log("gles2 profile: %08x %u", best[index], best_count[index]);
}

/* (its own thread: the game's may be the one stuck) */
static void *profile_thread(void *unused)
{
	(void)unused;
	for (;;)
	{
		sleep(10);
		profile_report();
	}
	return NULL;
}

static void profile_start(void)
{
	pthread_t thread;
	struct sigaction action;
	struct itimerval timer;

	if (!getenv("HALO_GLES2_PROFILE"))
		return;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = profile_signal;
	action.sa_flags = SA_SIGINFO | SA_RESTART;
	sigaction(SIGPROF, &action, NULL);
	timer.it_interval.tv_sec = 0;
	timer.it_interval.tv_usec = 10000;
	timer.it_value = timer.it_interval;
	setitimer(ITIMER_PROF, &timer, NULL);
	pthread_create(&thread, NULL, profile_thread, NULL);
	platform_log("gles2: profiling");
}

/* ---------- the layer */

#define LAYER_SAVE(name) real.name = halo_gl##name
#define LAYER_REPLACE(name) halo_gl##name = layer_##name

int console_gles2_layer(void)
{
	GLint unit;

	LAYER_SAVE(GetIntegerv);
	LAYER_SAVE(ActiveTexture);
	LAYER_SAVE(BindTexture);
	LAYER_SAVE(TexParameteri);
	LAYER_SAVE(TexParameteriv);
	LAYER_SAVE(TexParameterf);
	LAYER_SAVE(TexParameterfv);
	LAYER_SAVE(TexImage2D);
	LAYER_SAVE(TexSubImage2D);
	LAYER_SAVE(CompressedTexImage2D);
	LAYER_SAVE(BindFramebuffer);
	LAYER_SAVE(FramebufferTexture2D);
	LAYER_SAVE(FramebufferRenderbuffer);
	LAYER_SAVE(CheckFramebufferStatus);
	LAYER_SAVE(ReadPixels);
	LAYER_SAVE(BindBuffer);
	LAYER_SAVE(EnableVertexAttribArray);
	LAYER_SAVE(DisableVertexAttribArray);
	LAYER_SAVE(VertexAttribPointer);
	LAYER_SAVE(CreateShader);
	LAYER_SAVE(ShaderSource);
	LAYER_SAVE(CompileShader);
	LAYER_SAVE(AttachShader);
	LAYER_SAVE(LinkProgram);
	LAYER_SAVE(UseProgram);
	real_GenTextures = halo_glGenTextures;
	real_DeleteTextures = halo_glDeleteTextures;
	real.RenderbufferStorage = (__typeof__(real.RenderbufferStorage))SDL_GL_GetProcAddress("glRenderbufferStorage");
	real.GetFramebufferAttachmentParameteriv = (__typeof__(real.GetFramebufferAttachmentParameteriv))
		SDL_GL_GetProcAddress("glGetFramebufferAttachmentParameteriv");
	real.VertexAttrib4f = (__typeof__(real.VertexAttrib4f))SDL_GL_GetProcAddress("glVertexAttrib4f");
	real_CopyTexSubImage2D = (__typeof__(real_CopyTexSubImage2D))SDL_GL_GetProcAddress("glCopyTexSubImage2D");
	real.DiscardFramebufferEXT = host_gl_has_extension("GL_EXT_discard_framebuffer") ?
		(__typeof__(real.DiscardFramebufferEXT))SDL_GL_GetProcAddress("glDiscardFramebufferEXT") : NULL;
	if (!real.GetIntegerv || !real.ActiveTexture || !real.BindTexture || !real.TexParameteri || !real.TexImage2D ||
		!real.BindFramebuffer || !real.FramebufferTexture2D || !real.BindBuffer || !real.VertexAttribPointer ||
		!real.CreateShader || !real.ShaderSource || !real.CompileShader || !real.LinkProgram ||
		!real.RenderbufferStorage || !real.GetFramebufferAttachmentParameteriv || !real.VertexAttrib4f ||
		!real_GenTextures || !real_DeleteTextures)
	{
		platform_log("gles2: the driver lacks an ES 2 entry point");
		return 0;
	}

	LAYER_REPLACE(GetIntegerv);
	LAYER_REPLACE(ActiveTexture);
	LAYER_REPLACE(BindTexture);
	LAYER_REPLACE(GenTextures);
	halo_glDeleteTextures = layer_DeleteTextures_hook;
	LAYER_REPLACE(TexParameteri);
	LAYER_REPLACE(TexParameteriv);
	LAYER_REPLACE(TexParameterf);
	LAYER_REPLACE(TexParameterfv);
	LAYER_REPLACE(TexImage2D);
	LAYER_REPLACE(TexSubImage2D);
	LAYER_REPLACE(CompressedTexImage2D);
	LAYER_REPLACE(TexImage3D);
	LAYER_REPLACE(CompressedTexImage3D);
	LAYER_REPLACE(GenSamplers);
	LAYER_REPLACE(BindSampler);
	LAYER_REPLACE(SamplerParameteri);
	LAYER_REPLACE(SamplerParameterf);
	LAYER_REPLACE(SamplerParameterfv);
	LAYER_REPLACE(BindFramebuffer);
	LAYER_REPLACE(FramebufferTexture2D);
	LAYER_REPLACE(FramebufferRenderbuffer);
	LAYER_REPLACE(CheckFramebufferStatus);
	LAYER_REPLACE(ReadPixels);
	LAYER_REPLACE(BlitFramebuffer);
	LAYER_REPLACE(DrawBuffers);
	LAYER_REPLACE(ReadBuffer);
	LAYER_REPLACE(InvalidateFramebuffer);
	LAYER_REPLACE(RenderbufferStorageMultisample);
	LAYER_REPLACE(BindBuffer);
	LAYER_REPLACE(GenBuffers);
	LAYER_REPLACE(DeleteBuffers);
	LAYER_REPLACE(BufferData);
	LAYER_REPLACE(BufferSubData);
	LAYER_REPLACE(CopyBufferSubData);
	LAYER_REPLACE(BindBufferBase);
	LAYER_REPLACE(BindBufferRange);
	LAYER_REPLACE(GenVertexArrays);
	LAYER_REPLACE(BindVertexArray);
	LAYER_REPLACE(EnableVertexAttribArray);
	LAYER_REPLACE(DisableVertexAttribArray);
	LAYER_REPLACE(VertexAttribPointer);
	LAYER_REPLACE(VertexAttribIPointer);
	LAYER_REPLACE(VertexAttribI4ui);
	LAYER_REPLACE(DrawElementsBaseVertex);
	real_DrawArrays = halo_glDrawArrays;
	real_DrawElements = halo_glDrawElements;
	LAYER_REPLACE(DrawArrays);
	LAYER_REPLACE(DrawElements);
	LAYER_REPLACE(CreateShader);
	LAYER_REPLACE(ShaderSource);
	LAYER_REPLACE(CompileShader);
	LAYER_REPLACE(AttachShader);
	LAYER_REPLACE(LinkProgram);
	LAYER_REPLACE(UseProgram);
	LAYER_REPLACE(GenQueries);
	LAYER_REPLACE(BeginQuery);
	LAYER_REPLACE(EndQuery);
	LAYER_REPLACE(GetQueryObjectuiv);
	/* (ES 2 has no copy between images: the renderer blits, d3d8_gl.c) */
	halo_glCopyImageSubData = NULL;
	halo_glMemoryBarrier = NULL;

	real.GetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attribute_maximum);
	if (attribute_maximum > LAYER_ATTRIBUTES)
		attribute_maximum = LAYER_ATTRIBUTES;
	vertex_arrays[0].used = 1;
	for (unit = 0; unit < LAYER_TEXTURE_UNITS; unit++)
		unit_samplers[unit] = 0;
	platform_log("gles2: ES 2 filled out to ES 3 for the renderer (%d vertex attributes)", attribute_maximum);
	profile_start();
	return 1;
}
