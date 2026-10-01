package com.dongfang20101113.dchat

import android.app.Application
import com.dongfang20101113.dchat.net.DchatSession

/**
 * 应用入口。
 *
 * 这里只做一件关键的事：**初始化进程级会话**。
 *
 * 连接和聊天状态都放在 [DchatSession]（Application 作用域）里，而不是放在
 * Activity 作用域的 ViewModel 里——否则打开系统文件选择器、旋转屏幕、
 * 或系统因内存紧张重建 Activity 时，连接会被连带取消，
 * 用户看到的就是"打开文件上传几秒后自动掉线"。
 */
class DchatApp : Application() {
    override fun onCreate() {
        super.onCreate()
        DchatSession.init(this)
    }
}
