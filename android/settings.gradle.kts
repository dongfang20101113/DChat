// dchat Android 客户端 —— Gradle 设置
//
// 仓库顺序是**实测**排出来的，不是照抄模板：
//   这台机器上 dl.google.com 极不稳定（梯子开着也时通时断，实测 0/4 失败），
//   而国内镜像一直可达（阿里云 / 腾讯 / 华为都验证过能取到 AGP 及其 plugin marker）。
//   所以把国内镜像放第一位，官方源只作兜底 —— 这样梯子开或关都能构建。
pluginManagement {
    repositories {
        maven("https://maven.aliyun.com/repository/google")        // AGP / AndroidX
        maven("https://maven.aliyun.com/repository/public")        // Maven Central
        maven("https://maven.aliyun.com/repository/gradle-plugin") // Gradle 插件门户
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        maven("https://maven.aliyun.com/repository/google")
        maven("https://maven.aliyun.com/repository/public")
        google()
        mavenCentral()
    }
}

rootProject.name = "dchat-android"
include(":app")
