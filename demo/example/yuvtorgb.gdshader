shader_type canvas_item;

uniform sampler2D y_texture : filter_linear, repeat_disable;
uniform sampler2D uv_texture : filter_linear, repeat_disable;
uniform int color_space; // 0: BT.709, 1: BT.2020
uniform bool is_hdr;

void fragment() {
    float y = texture(y_texture, UV).r;
    vec2 uv = texture(uv_texture, UV).rg;

    vec3 yuv;
    // 修正 Limited Range 偏移量 (处理偏绿/发灰的关键)
    // 对于 8-bit NV12, Y 范围 16-235, UV 范围 16-240
    yuv.x = (y - (16.0 / 255.0)) * (255.0 / 219.0);
    yuv.y = uv.x - 0.5;
    yuv.z = uv.y - 0.5;

    vec3 rgb;
    if (color_space == 1) {
        // BT.2020 转换矩阵 (HDR)
        rgb.r = yuv.x + 1.4746 * yuv.z;
        rgb.g = yuv.x - 0.1645 * yuv.y - 0.5713 * yuv.z;
        rgb.b = yuv.x + 1.8814 * yuv.y;
    } else {
        // BT.709 转换矩阵 (SDR HD) - 解决偏绿问题的标准矩阵
        rgb.r = yuv.x + 1.5748 * yuv.z;
        rgb.g = yuv.x - 0.1873 * yuv.y - 0.4681 * yuv.z;
        rgb.b = yuv.x + 1.8556 * yuv.y;
    }

    // 如果是 HDR 内容，这里可以添加简单的 Tonemapping 或直接输出
    // 注意：Godot 的屏幕空间通常是 sRGB，HDR 渲染需要特定的 Viewport 设置
    COLOR = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}