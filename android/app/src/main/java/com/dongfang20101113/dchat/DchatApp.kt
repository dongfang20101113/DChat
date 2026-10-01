package com.dongfang20101113.dchat

import android.app.Application

/**
 * 应用入口。
 *
 * 目前不做任何初始化——连接状态、配置读写都放在各自的类里按需初始化，
 * 避免 Application 启动时做 IO 拖慢冷启动。
 */
class DchatApp : Application()
