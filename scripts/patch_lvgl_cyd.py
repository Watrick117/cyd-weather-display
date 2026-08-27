Import("env")

from pathlib import Path


project_dir = Path(env.subst("$PROJECT_DIR"))
source = project_dir / ".pio" / "libdeps" / env.subst("$PIOENV") / "LVGL_CYD" / "src" / "LVGL_CYD.cpp"

if source.exists():
    original = "#define DRAW_BUF_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT / 10 * (LV_COLOR_DEPTH / 8))"
    content = source.read_text(encoding="utf-8")
    content = content.replace(
        "#define DRAW_BUF_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT / 16 * (LV_COLOR_DEPTH / 8))",
        original,
    )
    content = content.replace(
        "uint32_t draw_buf[DRAW_BUF_SIZE / 4];",
        "uint32_t *draw_buf;",
    )
    content = content.replace(
        "lv_disp_t * display = lv_tft_espi_create(SCREEN_WIDTH, SCREEN_HEIGHT, draw_buf, sizeof(draw_buf));",
        "draw_buf = new uint32_t[DRAW_BUF_SIZE / 4];\n"
        "  lv_disp_t * display = lv_tft_espi_create(SCREEN_WIDTH, SCREEN_HEIGHT, draw_buf, DRAW_BUF_SIZE);",
    )
    source.write_text(content, encoding="utf-8")
