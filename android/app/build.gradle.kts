plugins {
    // 注意：AGP 9.0 起 **内置了 Kotlin 支持**，不能再应用 `org.jetbrains.kotlin.android`，
    // 否则报 "The 'org.jetbrains.kotlin.android' plugin is no longer required for Kotlin support since AGP 9.0"。
    // Compose 编译器插件仍然需要单独声明。
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "com.dongfang20101113.dchat"
    // 必须是 37.2：AndroidX core 1.19.1 / Compose 1.12.1 的 AAR 元数据要求
    // compileSdk >= 37（它建议的正是 37.2），用 36 会在 checkDebugAarMetadata 阶段直接失败。
    // AGP 9 用独立属性表达 API 的"小版本号"，所以是 compileSdk + compileSdkMinor 两个值，
    // 对应 SDK 里的 platforms/android-37.2。
    compileSdk = 37
    compileSdkMinor = 2

    defaultConfig {
        applicationId = "com.dongfang20101113.dchat"
        minSdk = 26          // Android 8.0：覆盖率高，且能用 java.time / 通知渠道等现代 API
        targetSdk = 37
        versionCode = 1
        versionName = "1.0.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
        debug {
            applicationIdSuffix = ".debug"
            versionNameSuffix = "-debug"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }

    buildFeatures {
        compose = true
    }

    // 单元测试要跑 Robolectric（离屏渲染截图），必须能读 Android 资源
    testOptions {
        unitTests {
            isIncludeAndroidResources = true
            isReturnDefaultValues = true
        }
    }

    packaging {
        resources.excludes += setOf(
            "/META-INF/{AL2.0,LGPL2.1}",
            "/META-INF/DEPENDENCIES",
        )
    }
}

// Kotlin 的 jvmTarget 由 AGP 内置 Kotlin 支持按 compileOptions 推导，
// 不需要（在 AGP 9 上也不能）再挂 `org.jetbrains.kotlin.android` 的顶层 kotlin{} 块。

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.datastore.preferences)
    implementation(libs.kotlinx.coroutines.android)

    val composeBom = platform(libs.androidx.compose.bom)
    implementation(composeBom)
    androidTestImplementation(composeBom)

    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.material.icons.extended)
    debugImplementation(libs.androidx.compose.ui.tooling)

    // ---- 单元测试 ----
    // 协议层与布局层都是纯逻辑，用普通 JUnit 即可（不需要 Android 运行时）
    testImplementation(libs.junit)
    testImplementation(libs.kotlinx.coroutines.test)

    // 截图验证：Robolectric 原生图形模式可以离屏渲染出真实像素
    testImplementation(libs.robolectric)
    testImplementation(libs.androidx.test.core)
    testImplementation(libs.androidx.test.ext.junit)
    testImplementation(libs.androidx.compose.ui.test.junit4)
    debugImplementation(libs.androidx.compose.ui.test.manifest)
}
