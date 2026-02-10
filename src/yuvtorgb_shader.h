#pragma once

static const char *YUV_SHADER_CODE = R"(
shader_type canvas_item;
render_mode unshaded; // Prevent lighting interference

uniform sampler2D tex_y : filter_linear, repeat_disable;
uniform sampler2D tex_u : filter_linear, repeat_disable; // Holds UV in NV12 mode
uniform sampler2D tex_v : filter_linear, repeat_disable; // Unused in NV12 mode

uniform bool is_semi_planar; // true = NV12, false = YUV420P
uniform int color_matrix_type; // 0 = BT.601, 1 = BT.709, 2 = BT.2020
uniform int color_range; // 0 = Limited (TV/MPEG), 1 = Full (PC/JPEG)

void fragment() {
	float y_raw = texture(tex_y, UV).r;
	float u_raw = 0.0;
	float v_raw = 0.0;

	if (is_semi_planar) {
		// NV12: UV are interleaved in the RG channels of the second texture
		vec2 uv_val = texture(tex_u, UV).rg;
		u_raw = uv_val.r;
		v_raw = uv_val.g;
	} else {
		// YUV420P: Planar
		u_raw = texture(tex_u, UV).r;
		v_raw = texture(tex_v, UV).r;
	}

	float y, u, v;

	// Range Adjustment
	// Limited: Y[16/255, 235/255], UV[16/255, 240/255]
	// Full: Y[0, 1], UV[0, 1]
	if (color_range == 0) {
		// Limited Range (MPEG)
		y = (y_raw - 16.0/255.0) * (255.0/219.0);
		u = (u_raw - 128.0/255.0) * (255.0/224.0);
		v = (v_raw - 128.0/255.0) * (255.0/224.0);
	} else {
		// Full Range (JPEG)
		y = y_raw;
		u = u_raw - 0.5;
		v = v_raw - 0.5;
	}

	// YUV to RGB Conversion
	// Coefficients based on standard ITU-R matrices (assuming normalized YUV input above)
	vec3 rgb = vec3(0.0);

	if (color_matrix_type == 1) { 
		// BT.709 (HDTV)
		// R = Y + 1.5748 * V
		// G = Y - 0.1873 * U - 0.4681 * V
		// B = Y + 1.8556 * U
		rgb.r = y + 1.5748 * v;
		rgb.g = y - 0.1873 * u - 0.4681 * v;
		rgb.b = y + 1.8556 * u;
	} else if (color_matrix_type == 2) {
		// BT.2020 (HDR/UHD) - Simple approximation for 8-bit pipeline
		rgb.r = y + 1.4746 * v;
		rgb.g = y - 0.16455 * u - 0.57135 * v;
		rgb.b = y + 1.8814 * u;
	} else {
		// BT.601 (SDTV/JPEG)
		rgb.r = y + 1.402 * v;
		rgb.g = y - 0.344136 * u - 0.714136 * v;
		rgb.b = y + 1.772 * u;
	}

	COLOR = vec4(rgb, 1.0);
}
)";
