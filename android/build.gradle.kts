// 根构建脚本：只声明插件，不在这里应用
plugins {
    alias(libs.plugins.android.application) apply false
    // AGP 9 内置 Kotlin，无需 kotlin.android 插件
    alias(libs.plugins.kotlin.compose) apply false
}
