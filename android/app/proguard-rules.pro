# dchat Android —— R8 / ProGuard 规则
#
# 说明：Compose、AndroidX、DataStore、kotlinx-coroutines 的规则由各库自带的
# consumer rules 提供，AGP 会自动合并。这里**只写本项目真正需要的几条**，
# 不重复抄网上的模板（多余的 keep 只会让包变大、还会掩盖真实的混淆问题）。

# ---- 保留行号：线上崩溃栈才能定位到具体代码行 ----
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile

# ---- 协议层的单例与枚举 ----
# ServerLine 是 sealed interface，R8 需要保留其子类结构；
# ChatState/Kind 这类被 `when` 穷举的枚举也必须完整。
-keep class com.dongfang20101113.dchat.protocol.ServerLine { *; }
-keep class com.dongfang20101113.dchat.protocol.ServerLine$* { *; }
-keepclassmembers enum com.dongfang20101113.dchat.** {
    public static **[] values();
    public static ** valueOf(java.lang.String);
}

# ---- 数据类：Compose 的稳定性推断会读 equals/hashCode ----
# 不保留的话理论上可能出现"状态变了但界面不重组"，这类问题极难排查，
# 所以对界面状态类和聊天条目显式保留。
-keepclassmembers class com.dongfang20101113.dchat.ui.** {
    <init>(...);
    *** component*();
    *** copy(...);
}
-keep class com.dongfang20101113.dchat.ui.ChatItem$* { *; }

# ---- kotlinx.coroutines ----
# 协程内部有大量反射与 volatile 字段访问
-dontwarn kotlinx.coroutines.**
-keepclassmembers class kotlinx.coroutines.** {
    volatile <fields>;
}

# ---- 只用于单元测试、不进 release 包的依赖 ----
-dontwarn org.robolectric.**
-dontwarn org.junit.**
-dontwarn androidx.test.**
