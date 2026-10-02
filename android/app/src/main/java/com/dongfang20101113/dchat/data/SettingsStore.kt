package com.dongfang20101113.dchat.data

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import com.dongfang20101113.dchat.protocol.DEFAULT_PORT
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map

private val Context.dataStore: DataStore<Preferences> by preferencesDataStore(name = "dchat-settings")

/** 上次连的地址和端口（和桌面端一样，只记住这两个，不记密码）。 */
data class ConnectionPrefs(val host: String = "127.0.0.1", val port: Int = DEFAULT_PORT)

/**
 * 设置持久化。
 *
 * **刻意只存地址和端口**——和桌面端一致：不保存密码、不保存上次的用户名。
 * 手机比电脑更容易丢，少存一点敏感信息少一分风险（而且协议本身是明文的）。
 */
class SettingsStore(private val context: Context) {

    val connection: Flow<ConnectionPrefs> = context.dataStore.data.map { prefs ->
        ConnectionPrefs(
            host = prefs[KEY_HOST] ?: "127.0.0.1",
            port = prefs[KEY_PORT] ?: DEFAULT_PORT,
        )
    }

    suspend fun saveConnection(host: String, port: Int) {
        context.dataStore.edit { prefs ->
            prefs[KEY_HOST] = host
            prefs[KEY_PORT] = port
        }
    }

    // ------------------------------------------------------------------
    // TOFU：记下服务器指纹
    // ------------------------------------------------------------------

    /**
     * 取某个服务器上次记下的公钥指纹（没记过返回 null）。
     *
     * 键是 `主机:端口`——同一台机器的不同端口算不同的服务器，
     * 因为它们完全可能是两个不同的服务端进程、两把不同的密钥。
     */
    suspend fun trustedFingerprint(host: String, port: Int): String? =
        context.dataStore.data.map { it[fingerprintKey(host, port)] }.first()

    /** 记下（或更新）某个服务器的指纹。 */
    suspend fun saveTrustedFingerprint(host: String, port: Int, fingerprint: String) {
        context.dataStore.edit { prefs ->
            prefs[fingerprintKey(host, port)] = fingerprint
        }
    }

    /** 忘掉某个服务器的指纹（用户核对后确认换过密钥时用）。 */
    suspend fun forgetTrustedFingerprint(host: String, port: Int) {
        context.dataStore.edit { prefs ->
            prefs.remove(fingerprintKey(host, port))
        }
    }

    private fun fingerprintKey(host: String, port: Int) =
        stringPreferencesKey("fingerprint:$host:$port")

    private companion object {
        val KEY_HOST = stringPreferencesKey("host")
        val KEY_PORT = intPreferencesKey("port")
    }
}
