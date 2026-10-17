// NBG cell quad shader (ADR-014). Atlas coords, palette base, priority and special color calc bit come in per quad as vertex attributes.
#ifndef RSTV_NBGQ_SHADERS_H
#define RSTV_NBGQ_SHADERS_H

// position: screen pixels, uv: atlas texels, attr: (palette base, priority, scf, 0)
// scale: (2/texture width, 2/texture height, 1/atlas, 0). Screen line 0 = FBO row 0 (NDC y = -1)
static const char *s_vs_q =
	"void main(float2 position, float2 uv, float4 attr, uniform float4 scale,\n"
	"  float4 out vPosition : POSITION, float2 out vUV : TEXCOORD0, float4 out vAttr : TEXCOORD1)\n"
	"{\n"
	"  vPosition = float4(position * scale.xy - 1.0, 0.0, 1.0);\n"
	"  vUV = uv * scale.z;\n"
	"  vAttr = attr;\n"
	"}\n";

// qcfg: (transparency enable, special color calc mode, ratio usable (alpha != 63), 0), qcodeLo/Hi: special code bits 0..7
// output is layer texture format: rgb = color before offset, a = key (priority * 8 + layer order qcfg.w, +128 if ratio used, 0 if none)
static const char *s_fs_q =
	"float4 lout(float3 c, float prio, float flag, float idx) { return (prio < 0.5) ? float4(0.0, 0.0, 0.0, 0.0) : float4(c, (prio * 8.0 + idx + ((flag > 0.5) ? 128.0 : 0.0)) / 255.0); }\n"
	"float lut8(float i, float4 lo, float4 hi) {\n"
	"  float4 a = step(abs(float4(i, i, i, i) - float4(0.0, 1.0, 2.0, 3.0)), float4(0.5, 0.5, 0.5, 0.5));\n"
	"  float4 b = step(abs(float4(i, i, i, i) - float4(4.0, 5.0, 6.0, 7.0)), float4(0.5, 0.5, 0.5, 0.5));\n"
	"  return dot(a, lo) + dot(b, hi);\n"
	"}\n"
	"float4 main(float2 vUV : TEXCOORD0, float4 vAttr : TEXCOORD1, uniform sampler2D tAtlas, uniform sampler2D tPal,\n"
	"  uniform float4 qcfg, uniform float4 qcodeLo, uniform float4 qcodeHi) : COLOR\n"
	"{\n"
	"  float idx = floor(tex2D(tAtlas, vUV).x * 255.0 + 0.5);\n"
	"  if (idx < 0.5 && qcfg.x > 0.5) return float4(0.0, 0.0, 0.0, 0.0);\n"
	"  float pa = floor(vAttr.x + 0.5) + idx;\n"
	"  if (pa >= 2048.0) pa -= 2048.0;\n"
	"  float4 pc = tex2D(tPal, float2((pa + 0.5) / 2048.0, 0.5));\n"
	"  float scm = qcfg.y;\n"
	"  float use = 1.0;\n"
	"  if ((scm > 0.5 && scm < 2.5) && vAttr.z < 0.5) use = 0.0;\n"
	"  else if (scm > 1.5 && scm < 2.5) { if (lut8(floor(fmod(idx, 16.0) / 2.0), qcodeLo, qcodeHi) < 0.5) use = 0.0; }\n"
	"  else if (scm > 2.5 && pc.w < 0.5) use = 0.0;\n"
	"  float flag = (use > 0.5 && qcfg.z > 0.5) ? 8.0 : 0.0;\n"
	"  return lout(pc.xyz, floor(vAttr.y + 0.5), flag, qcfg.w);\n"
	"}\n";

// RGB555 cell quads (cn 3): same RGB atlas as RBG0 (1024x1024, char unit u -> ((u & 127) * 8, (u >> 7) * 8)).
// special color calc same as rbgq_shade (dot = red 5 bits, MSB mode unused)
static const char *s_fs_qrgb =
	"float4 lout(float3 c, float prio, float flag, float idx) { return (prio < 0.5) ? float4(0.0, 0.0, 0.0, 0.0) : float4(c, (prio * 8.0 + idx + ((flag > 0.5) ? 128.0 : 0.0)) / 255.0); }\n"
	"float lut8(float i, float4 lo, float4 hi) {\n"
	"  float4 a = step(abs(float4(i, i, i, i) - float4(0.0, 1.0, 2.0, 3.0)), float4(0.5, 0.5, 0.5, 0.5));\n"
	"  float4 b = step(abs(float4(i, i, i, i) - float4(4.0, 5.0, 6.0, 7.0)), float4(0.5, 0.5, 0.5, 0.5));\n"
	"  return dot(a, lo) + dot(b, hi);\n"
	"}\n"
	"float4 main(float2 vUV : TEXCOORD0, float4 vAttr : TEXCOORD1, uniform sampler2D tRgb,\n"
	"  uniform float4 qcfg, uniform float4 qcodeLo, uniform float4 qcodeHi) : COLOR\n"
	"{\n"
	"  float4 c = tex2D(tRgb, vUV);\n"
	"  if (c.w < 0.5 && qcfg.x > 0.5) return float4(0.0, 0.0, 0.0, 0.0);\n"
	"  float dotv = floor(c.x * 255.0 + 0.5) / 8.0;\n"
	"  float scm = qcfg.y;\n"
	"  float use = 1.0;\n"
	"  if ((scm > 0.5 && scm < 2.5) && vAttr.z < 0.5) use = 0.0;\n"
	"  else if (scm > 1.5 && scm < 2.5) { if (lut8(floor(fmod(dotv, 16.0) / 2.0), qcodeLo, qcodeHi) < 0.5) use = 0.0; }\n"
	"  else if (scm > 2.5) use = 0.0;\n"
	"  float flag = (use > 0.5 && qcfg.z > 0.5) ? 8.0 : 0.0;\n"
	"  return lout(c.xyz, floor(vAttr.y + 0.5), flag, qcfg.w);\n"
	"}\n"
;

#endif
