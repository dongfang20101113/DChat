package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.protocol.TrustDecision
import com.dongfang20101113.dchat.protocol.decideTrust
import com.dongfang20101113.dchat.protocol.describeTrust
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * TOFU（首次使用即信任）的判定逻辑。
 *
 * 为什么这段逻辑值得单独测：**悄悄接受新指纹等于 TOFU 完全没做**。
 * 如果"指纹变了"被错误地判成"一致"，用户永远不会看到警告，
 * 而这正是中间人攻击唯一会被发现的时机。所以这里的每个分支都要钉死。
 */
class TrustTest {

    private val fpA = "0D:5E:CE:C4:BD:AE:76:83:81:2F:1A:14:9F:03:15:29"
    private val fpB = "A6:D9:56:01:8D:57:34:E5:60:20:74:D4:84:8D:B7:3F"

    @Test
    fun `没加密时判定为未加密`() {
        assertEquals(TrustDecision.NotEncrypted, decideTrust(known = null, current = null))
        assertEquals(TrustDecision.NotEncrypted, decideTrust(known = fpA, current = null))
        assertEquals(TrustDecision.NotEncrypted, decideTrust(known = null, current = ""))
        // 以前记过指纹、这次却没加密 —— 很可能是降级攻击，绝不能当成"一致"
        assertEquals(TrustDecision.NotEncrypted, decideTrust(known = fpA, current = ""))
    }

    @Test
    fun `第一次见到就记下来`() {
        val decision = decideTrust(known = null, current = fpA)
        assertEquals(TrustDecision.FirstUse(fpA), decision)
    }

    @Test
    fun `指纹没变就是已确认`() {
        assertEquals(TrustDecision.Unchanged(fpA), decideTrust(known = fpA, current = fpA))
    }

    @Test
    fun `★ 指纹变了必须判定为 Changed`() {
        val decision = decideTrust(known = fpA, current = fpB)
        assertTrue("指纹不同绝不能判成一致，实际 $decision", decision is TrustDecision.Changed)
        decision as TrustDecision.Changed
        assertEquals(fpA, decision.previous)
        assertEquals(fpB, decision.current)
    }

    @Test
    fun `大小写不同不算变化`() {
        assertEquals(
            TrustDecision.Unchanged(fpA),
            decideTrust(known = fpA.lowercase(), current = fpA.uppercase()),
        )
    }

    @Test
    fun `差一个字符也算变化`() {
        val slightlyDifferent = fpA.dropLast(1) + "X"
        assertTrue(decideTrust(fpA, slightlyDifferent) is TrustDecision.Changed)
    }

    @Test
    fun `空字符串等同于没记过`() {
        assertTrue(decideTrust(known = "", current = fpA) is TrustDecision.FirstUse)
    }

    // ------------------------------------------------------------------
    // 给用户看的文案
    // ------------------------------------------------------------------

    @Test
    fun `文案要能区分各种状态`() {
        assertTrue(describeTrust(TrustDecision.NotEncrypted).contains("未加密"))
        assertTrue(describeTrust(TrustDecision.FirstUse(fpA)).contains(fpA))
        assertTrue(describeTrust(TrustDecision.Unchanged(fpA)).contains("已确认"))

        val changed = describeTrust(TrustDecision.Changed(fpA, fpB))
        assertTrue("变了的时候必须显眼", changed.contains("⚠"))
        assertTrue("新旧指纹都要给出来，用户才能核对", changed.contains(fpA) && changed.contains(fpB))
        assertTrue("要提醒用户去核对，而不是自己决定", changed.contains("核对"))
    }

    @Test
    fun `首次连接的文案不该吓唬人`() {
        val text = describeTrust(TrustDecision.FirstUse(fpA))
        assertTrue("首次连接是正常情况，不该用警告符号", !text.contains("⚠"))
    }

    @Test
    fun `未加密的文案要让用户知道风险`() {
        val text = describeTrust(TrustDecision.NotEncrypted)
        assertTrue("要说清后果：明文、公网可见", text.contains("明文") || text.contains("公网"))
    }
}
