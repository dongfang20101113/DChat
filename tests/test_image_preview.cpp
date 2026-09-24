// 图片/视频预览的类型判断测试（纯逻辑，不碰系统缩略图接口）
#include <cstdio>

#include "image_preview.h"

namespace {
int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("  ok   %s\n", what);
    } else {
        ++g_failures;
        std::printf("  FAIL %s\n", what);
    }
}
}  // namespace

int main() {
    std::printf("== dchat image preview tests ==\n");

    std::printf("[1] 图片类型\n");
    check(dchat::IsImageFile("photo.png"), "png");
    check(dchat::IsImageFile("photo.JPG"), "扩展名大小写不敏感");
    check(dchat::IsImageFile("d:/downloads/报告.jpeg"), "带路径、中文名也行");
    check(dchat::IsImageFile("a.bmp") && dchat::IsImageFile("a.webp") && dchat::IsImageFile("a.gif"),
          "bmp / webp / gif 都算图片");
    check(!dchat::IsImageFile("note.txt"), "txt 不是图片");
    check(!dchat::IsImageFile("photo"), "没有扩展名不是图片");
    check(!dchat::IsImageFile("png"), "只有扩展名本身（没点）不算");

    std::printf("[2] 视频类型\n");
    check(dchat::IsVideoFile("movie.mp4"), "mp4");
    check(dchat::IsVideoFile("MOVEI.MKV"), "mkv，大小写不敏感");
    check(dchat::IsVideoFile("clip.webm") && dchat::IsVideoFile("old.avi"), "webm / avi");
    check(!dchat::IsVideoFile("movie.mp4.txt"), "以 .txt 结尾就不算视频");
    check(!dchat::IsImageFile("movie.mp4"), "视频不算图片");

    std::printf("[3] 能不能预览\n");
    check(dchat::IsPreviewableFile("a.png") && dchat::IsPreviewableFile("a.mp4"),
          "图片和视频都能预览");
    check(!dchat::IsPreviewableFile("a.zip") && !dchat::IsPreviewableFile("程序.exe"),
          "压缩包 / 可执行文件不预览");

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
