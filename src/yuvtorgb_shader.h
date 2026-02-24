#pragma once

static const char *YUV_SHADER_CODE = R"(
shader_type canvas_item;
render_mode unshaded; // Prevent lighting interference

uniform sampler2D tex_y : filter_linear, repeat_disable;
uniform sampler2D tex_u : filter_linear, repeat_disable;
uniform sampler2D tex_v : filter_linear, repeat_disable; // Unused in NV12 mode

uniform bool is_semi_planar; // true = NV12, false = YUV420P
uniform int color_matrix_type; // 0 = BT.601, 1 = BT.709, 2 = BT.2020
uniform int color_range; // 0 = Limited (TV/MPEG), 1 = Full (PC/JPEG)
uniform bool swap_uv; // Fix for NV12/NV21 mismatch (Red->Blue/Green artifacts)

void fragment() {
	// Read Y from red channel of R8 texture (standard for single-channel textures)
	float y_raw = texture(tex_y, UV).r;
	float u_raw = 0.5;
	float v_raw = 0.5;

	if (is_semi_planar) {
		vec2 uv_val = texture(tex_u, UV).rg;
		// Standard NV12 (U,V). If colors swapped, swap_uv will flip them.
		u_raw = uv_val.r;
		v_raw = uv_val.g;
		
		if (swap_uv) {
			float temp = u_raw;
			u_raw = v_raw;
			v_raw = temp;
		}
	} else {
		// YUV420P: Planar
		// Check for swap_uv also in planar mode, just in case U/V planes are swapped in data
		if (swap_uv) {
			u_raw = texture(tex_v, UV).r;
			v_raw = texture(tex_u, UV).r;
		} else {
			u_raw = texture(tex_u, UV).r;
			v_raw = texture(tex_v, UV).r;
		}
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
	vec3 rgb = vec3(0.0);

	// Standard conversion matrix
	if (color_matrix_type == 1) { 
		// BT.709 (HDTV)
		rgb.r = y + 1.5748 * v;
		rgb.g = y - 0.1873 * u - 0.4681 * v;
		rgb.b = y + 1.8556 * u;
	} else if (color_matrix_type == 2) {
		// BT.2020 (HDR/UHD)
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
