// macOS 客户端的图形界面（Cocoa / Objective-C++）。
//
// ⚠️ 这个文件**从未被编译过**：开发机上没有 macOS SDK，zig 也不带 AppKit 头。
//    所以这里刻意只用最保守、最标准的 AppKit API（NSWindow / NSTextView /
//    NSTextField / NSButton / NSStackView），把"用了冷门 API 而编不过"的风险压到最低。
//    业务逻辑一律放在能验证的 chat_core 里，这一层只做"把字符串画出来"。
//
// 线程约定：ChatCore 的回调在**接收线程**上触发，所有界面更新都 dispatch 回主线程。
#import <Cocoa/Cocoa.h>

#include <memory>
#include <string>
#include <vector>

#include "chat_core.h"
#include "chat_color.h"
#include "protocol.h"

// ---------------------------------------------------------------------------
// 颜色：把色码解析出来的 RGB 变成 NSColor，并把正文拼成 NSAttributedString
// ---------------------------------------------------------------------------
static NSColor* ColorFromRgb(uint32_t rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1.0];
}

/// 默认正文色 / 时间戳灰 / 系统提示青 / 错误红 / @我 的高亮黄
static NSColor* BodyColor(void) { return [NSColor labelColor]; }
static NSColor* DimColor(void) { return [NSColor secondaryLabelColor]; }
static NSColor* SystemColor(void) { return [NSColor systemTealColor]; }
static NSColor* ErrorColor(void) { return [NSColor systemRedColor]; }
static NSColor* MentionColor(void) { return [NSColor systemOrangeColor]; }

// ---------------------------------------------------------------------------
// 主窗口
// ---------------------------------------------------------------------------
@interface DChatWindowController : NSObject <NSApplicationDelegate> {
@private
    // ---- 连接界面 ----
    NSWindow* _connectWindow;
    NSTextField* _hostField;
    NSTextField* _portField;
    NSTextField* _userField;
    NSSecureTextField* _passField;
    NSButton* _registerCheck;
    NSTextField* _connectStatus;

    // ---- 聊天界面 ----
    NSWindow* _chatWindow;
    NSTextView* _transcript;   // 聊天记录（只读）
    NSTextField* _input;       // 输入框
    NSTextField* _statusBar;   // 底部状态
    NSTextView* _memberView;   // 右侧在线成员（用文本视图而不是 NSTableView：
                               // 数据源协议写错在这里无法编译验证，文本视图没有这个风险）

    std::unique_ptr<dchat::ChatCore> _core;
    BOOL _loggedIn;
}
- (void)showConnectWindow;
- (void)showChatWindow;
@end

// ---------------------------------------------------------------------------
// 把 C++ 回调转成 Cocoa：实现 ChatCoreDelegate，全部 dispatch 到主线程
// ---------------------------------------------------------------------------
namespace {

class CocoaDelegate : public dchat::ChatCoreDelegate {
public:
    __weak DChatWindowController* controller = nil;

    void OnChatMessage(const dchat::ChatMessage& message) override {
        if (!message.valid) return;
        // 附件卡片 / @我 / 色码这些都在这层决定怎么显示，核心只管给数据
        DChatWindowController* strong = controller;
        if (!strong) return;
        std::string text = message.text;
        int kind = static_cast<int>(message.kind);
        std::string nick = message.nick;
        std::string time = message.time;
        bool mention = message.mentionMe;
        bool hasColor = message.hasColor;
        std::string fileId = message.fileId;
        dispatch_async(dispatch_get_main_queue(), ^{
          [strong appendLineWithText:[NSString stringWithUTF8String:text.c_str()]
                                nick:[NSString stringWithUTF8String:nick.c_str()]
                                time:[NSString stringWithUTF8String:time.c_str()]
                                kind:kind
                             mention:mention
                            hasColor:hasColor
                              fileId:[NSString stringWithUTF8String:fileId.c_str()]];
        });
    }

    void OnConnected(bool encrypted, const std::string& fingerprint,
                     const dchat::TrustDecision& trust) override {
        const std::string text =
            encrypted ? (dchat::DescribeTrust(trust) + "（" + fingerprint + "）")
                      : std::string("这次连接没有加密");
        DChatWindowController* strong = controller;
        if (!strong) return;
        const std::string copy = text;
        dispatch_async(dispatch_get_main_queue(), ^{
          [strong appendSystemText:[NSString stringWithUTF8String:copy.c_str()]];
          [strong setStatus:[NSString stringWithUTF8String:copy.c_str()]];
        });
    }

    void OnLoggedIn(const std::string& nick) override {
        DChatWindowController* strong = controller;
        if (!strong) return;
        const std::string copy = nick;
        dispatch_async(dispatch_get_main_queue(), ^{
          [strong markLoggedIn:[NSString stringWithUTF8String:copy.c_str()]];
        });
    }

    void OnDisconnected(const std::string& reason) override {
        DChatWindowController* strong = controller;
        if (!strong) return;
        const std::string copy = reason;
        dispatch_async(dispatch_get_main_queue(), ^{
          [strong appendSystemText:[NSString stringWithUTF8String:copy.c_str()]];
          [strong setStatus:@"已断开"];
        });
    }

    void OnOnlineNicks(const std::vector<std::string>& nicks) override {
        std::string list;
        for (std::size_t i = 0; i < nicks.size(); ++i) {
            if (i) list += "\n";
            list += nicks[i];
        }
        DChatWindowController* strong = controller;
        if (!strong) return;
        const std::string copy = list;
        dispatch_async(dispatch_get_main_queue(), ^{
          [strong setMembers:[NSString stringWithUTF8String:copy.c_str()]];
        });
    }

    void OnTransferProgress(const std::string& text) override {
        DChatWindowController* strong = controller;
        if (!strong) return;
        const std::string copy = text;
        dispatch_async(dispatch_get_main_queue(), ^{
          [strong appendSystemText:[NSString stringWithUTF8String:copy.c_str()]];
        });
    }

    void OnColorSettingChanged(bool enabled, const std::string& rawRest) override {
        (void)rawRest;
        DChatWindowController* strong = controller;
        if (!strong) return;
        dispatch_async(dispatch_get_main_queue(), ^{
          [strong setColorEnabled:enabled ? YES : NO];
        });
    }

    void OnVoiceCommand(const std::string& arg) override {
        // Cocoa 版的录音还没做（macos/main.mm 本来就没法在本机编译验证，
        // 不想再往里塞没验证过的音频采集代码）。如实告诉用户，别让他以为是坏了。
        (void)arg;
        DChatWindowController* strong = controller;
        if (!strong) return;
        dispatch_async(dispatch_get_main_queue(), ^{
          [strong appendSystemText:@"这个界面暂不支持录音（终端版可以：/voice）"];
        });
    }
};

CocoaDelegate g_delegate;

}  // namespace

// NSTextField 的回车代理：输入框按回车就发出去
@interface DChatWindowController () <NSTextFieldDelegate>
@end

@implementation DChatWindowController

- (instancetype)init {
    self = [super init];
    if (self) {
        _loggedIn = NO;
        _core = std::make_unique<dchat::ChatCore>(&g_delegate);
        g_delegate.controller = self;

        // TOFU 指纹备忘放到用户目录，和终端版（当前目录）分开，
        // 免得换个工作目录就"重新第一次信任"。
        NSString* support = [NSSearchPathForDirectoriesInDomains(
            NSApplicationSupportDirectory, NSUserDomainMask, YES) firstObject];
        if (support) {
            NSString* dir = [support stringByAppendingPathComponent:@"dchat"];
            [[NSFileManager defaultManager] createDirectoryAtPath:dir
                                      withIntermediateDirectories:YES
                                                       attributes:nil
                                                            error:nil];
            NSString* file = [dir stringByAppendingPathComponent:@"known-servers.txt"];
            _core->SetKnownServersPath(file.UTF8String);
        }
    }
    return self;
}

#pragma mark - 连接窗口

- (NSTextField*)labelWithText:(NSString*)text frame:(NSRect)frame {
    NSTextField* field = [[NSTextField alloc] initWithFrame:frame];
    field.stringValue = text;
    field.editable = NO;
    field.bezeled = NO;
    field.drawsBackground = NO;
    field.selectable = NO;
    return field;
}

- (NSTextField*)editableField:(NSString*)value frame:(NSRect)frame {
    NSTextField* field = [[NSTextField alloc] initWithFrame:frame];
    field.stringValue = value;
    return field;
}

- (void)showConnectWindow {
    NSRect frame = NSMakeRect(0, 0, 420, 300);
    _connectWindow = [[NSWindow alloc] initWithContentRect:frame
                                                 styleMask:(NSWindowStyleMaskTitled |
                                                            NSWindowStyleMaskClosable)
                                                   backing:NSBackingStoreBuffered
                                                     defer:NO];
    _connectWindow.title = @"连接 dchat";
    NSView* content = _connectWindow.contentView;

    [content addSubview:[self labelWithText:@"服务器" frame:NSMakeRect(24, 250, 80, 22)]];
    _hostField = [self editableField:@"127.0.0.1" frame:NSMakeRect(110, 248, 280, 24)];
    [content addSubview:_hostField];

    [content addSubview:[self labelWithText:@"端口" frame:NSMakeRect(24, 214, 80, 22)]];
    _portField = [self editableField:@"5555" frame:NSMakeRect(110, 212, 120, 24)];
    [content addSubview:_portField];

    [content addSubview:[self labelWithText:@"用户名" frame:NSMakeRect(24, 178, 80, 22)]];
    _userField = [self editableField:@"" frame:NSMakeRect(110, 176, 280, 24)];
    [content addSubview:_userField];

    [content addSubview:[self labelWithText:@"密码" frame:NSMakeRect(24, 142, 80, 22)]];
    _passField = [[NSSecureTextField alloc] initWithFrame:NSMakeRect(110, 140, 280, 24)];
    [content addSubview:_passField];

    _registerCheck = [NSButton checkboxWithTitle:@"没有账号，注册一个新账号"
                                          target:nil
                                          action:nil];
    _registerCheck.frame = NSMakeRect(110, 106, 280, 24);
    [content addSubview:_registerCheck];

    NSButton* connect = [NSButton buttonWithTitle:@"连接"
                                           target:self
                                           action:@selector(onConnectClicked:)];
    connect.frame = NSMakeRect(110, 66, 100, 32);
    connect.keyEquivalent = @"\r";
    [content addSubview:connect];

    NSButton* cancel = [NSButton buttonWithTitle:@"退出"
                                          target:NSApp
                                          action:@selector(terminate:)];
    cancel.frame = NSMakeRect(220, 66, 100, 32);
    [content addSubview:cancel];

    _connectStatus = [self labelWithText:@"" frame:NSMakeRect(24, 24, 372, 34)];
    _connectStatus.textColor = DimColor();
    _connectStatus.lineBreakMode = NSLineBreakByWordWrapping;
    [content addSubview:_connectStatus];

    [_connectWindow center];
    [_connectWindow makeKeyAndOrderFront:nil];
    [_connectWindow makeFirstResponder:_hostField];
}

- (void)onConnectClicked:(id)sender {
    (void)sender;
    const char* host = _hostField.stringValue.UTF8String;
    const char* user = _userField.stringValue.UTF8String;
    const char* pass = _passField.stringValue.UTF8String;

    _connectStatus.stringValue = @"正在连接…";
    NSTextField* status = _connectStatus;
    NSTextField* hostField = _hostField;
    NSTextField* portField = _portField;
    NSTextField* userField = _userField;
    NSTextField* passField = _passField;
    NSButton* registerCheck = _registerCheck;
    DChatWindowController* me = self;

    // connect() 会阻塞（含握手），放到后台线程，别卡住界面
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      dchat::ConnectOptions options;
      options.host = host ? host : "";
      options.port = portField.intValue > 0 ? (int)portField.intValue : 5555;
      options.user = user ? user : "";
      options.password = pass ? pass : "";
      options.wantRegister = registerCheck.state == NSControlStateValueOn;

      std::string error;
      const bool ok = me->_core->Connect(options, &error);
      dispatch_async(dispatch_get_main_queue(), ^{
        if (!ok) {
          status.stringValue = [NSString stringWithFormat:@"连接失败：%s",
                                                          error.empty() ? "未知原因"
                                                                        : error.c_str()];
          return;
        }
        hostField.enabled = NO;
        passField.stringValue = @"";
        [me showChatWindow];
      });
    });
}

#pragma mark - 聊天窗口

- (void)showChatWindow {
    NSRect frame = NSMakeRect(0, 0, 900, 620);
    _chatWindow = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:(NSWindowStyleMaskTitled |
                                                         NSWindowStyleMaskClosable |
                                                         NSWindowStyleMaskResizable |
                                                         NSWindowStyleMaskMiniaturizable)
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    _chatWindow.title = @"dchat";
    NSView* content = _chatWindow.contentView;

    // 聊天记录：只读、可选中、能滚。用 NSTextView 自己配滚动视图，
    // 不走 NSTableView 的数据源协议（那部分只能编译验证，这里用不到）。
    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 70, 690, 520)];
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.hasVerticalScroller = YES;
    _transcript = [[NSTextView alloc] initWithFrame:scroll.bounds];
    _transcript.editable = NO;
    _transcript.richText = YES;
    _transcript.autoresizingMask = NSViewWidthSizable;
    _transcript.font = [NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightRegular];
    _transcript.textContainerInset = NSMakeSize(10, 8);
    scroll.documentView = _transcript;
    [content addSubview:scroll];

    // 右侧在线成员
    NSScrollView* memberScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(690, 70, 210, 520)];
    memberScroll.autoresizingMask = NSViewHeightSizable | NSViewMinXMargin;
    memberScroll.hasVerticalScroller = YES;
    _memberView = [[NSTextView alloc] initWithFrame:memberScroll.bounds];
    _memberView.editable = NO;
    _memberView.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    _memberView.textContainerInset = NSMakeSize(8, 8);
    memberScroll.documentView = _memberView;
    [content addSubview:memberScroll];

    // 输入框
    _input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 34, 780, 30)];
    _input.autoresizingMask = NSViewWidthSizable;
    _input.placeholderString = @"说点什么…（回车发送，/help 看指令）";
    _input.delegate = self;
    [content addSubview:_input];

    NSButton* send = [NSButton buttonWithTitle:@"发送"
                                        target:self
                                        action:@selector(onSendClicked:)];
    send.frame = NSMakeRect(790, 34, 110, 30);
    send.autoresizingMask = NSViewMinXMargin;
    [content addSubview:send];

    // 底部状态栏
    _statusBar = [self labelWithText:@"正在登录…" frame:NSMakeRect(10, 8, 880, 20)];
    _statusBar.textColor = DimColor();
    _statusBar.font = [NSFont systemFontOfSize:12];
    _statusBar.autoresizingMask = NSViewWidthSizable;
    [content addSubview:_statusBar];

    [_chatWindow center];
    [_chatWindow makeKeyAndOrderFront:nil];
    [_chatWindow makeFirstResponder:_input];
    [_connectWindow orderOut:nil];
}

- (void)onSendClicked:(id)sender {
    (void)sender;
    [self sendCurrentInput];
}

// NSTextFieldDelegate：回车发送
- (void)controlTextDidEndEditing:(NSNotification*)note {
    if ([[note.userInfo objectForKey:@"NSTextMovement"] intValue] == NSReturnTextMovement) {
        [self sendCurrentInput];
    }
}

- (void)sendCurrentInput {
    NSString* raw = _input.stringValue;
    if (raw.length == 0) return;
    const std::string text = raw.UTF8String ? std::string(raw.UTF8String) : std::string();
    if (text.empty()) return;
    _input.stringValue = @"";
    // 逻辑全在 core 里（含本地指令派发），界面只负责把字符串交过去
    _core->SubmitInput(text);
}

#pragma mark - 供 C++ 回调调用的界面动作（都在主线程上）

- (void)appendLineWithText:(NSString*)text
                      nick:(NSString*)nick
                      time:(NSString*)time
                      kind:(int)kind
                   mention:(BOOL)mention
                  hasColor:(BOOL)hasColor
                    fileId:(NSString*)fileId {
    (void)fileId;
    NSMutableAttributedString* line = [[NSMutableAttributedString alloc] init];

    NSDictionary* dim = @{NSForegroundColorAttributeName : DimColor()};
    if (time.length) {
        [line appendAttributedString:[[NSAttributedString alloc]
                                         initWithString:[time stringByAppendingString:@" "]
                                             attributes:dim]];
    }

    switch (static_cast<dchat::ChatMessage::Kind>(kind)) {
        case dchat::ChatMessage::Kind::Say:
        case dchat::ChatMessage::Kind::Own: {
            NSDictionary* nickAttr = @{
                NSForegroundColorAttributeName : mention ? MentionColor() : SystemColor(),
                NSFontAttributeName : [NSFont boldSystemFontOfSize:13]
            };
            [line appendAttributedString:[[NSAttributedString alloc]
                                             initWithString:[NSString stringWithFormat:@"<%@> ",
                                                                                       nick]
                                                 attributes:nickAttr]];
            // 正文：有色码就按段上色（和 Windows / Linux / 安卓同一套解析）
            if (hasColor && _core->ColorEnabled()) {
                const std::string raw = text.UTF8String;
                for (const dchat::ColorSegment& segment :
                     dchat::ParseColorSegments(raw, 0xD0D0D0, true)) {
                    NSColor* color = segment.hasColor ? ColorFromRgb(segment.rgb) : BodyColor();
                    NSString* piece =
                        [NSString stringWithUTF8String:segment.text.c_str()] ?: @"";
                    [line appendAttributedString:[[NSAttributedString alloc]
                                                     initWithString:piece
                                                         attributes:@{
                                                             NSForegroundColorAttributeName : color
                                                         }]];
                }
            } else {
                [line appendAttributedString:[[NSAttributedString alloc]
                                                 initWithString:text
                                                     attributes:@{
                                                         NSForegroundColorAttributeName : BodyColor()
                                                     }]];
            }
            break;
        }
        case dchat::ChatMessage::Kind::Error:
            [line appendAttributedString:[[NSAttributedString alloc]
                                             initWithString:[@"[错误] " stringByAppendingString:text]
                                                 attributes:@{
                                                     NSForegroundColorAttributeName : ErrorColor()
                                                 }]];
            break;
        case dchat::ChatMessage::Kind::FileOffer:
            [line appendAttributedString:[[NSAttributedString alloc]
                                             initWithString:text
                                                 attributes:@{
                                                     NSForegroundColorAttributeName : MentionColor()
                                                 }]];
            break;
        case dchat::ChatMessage::Kind::System:
            [line appendAttributedString:[[NSAttributedString alloc]
                                             initWithString:[@"[系统] " stringByAppendingString:text]
                                                 attributes:@{
                                                     NSForegroundColorAttributeName : SystemColor()
                                                 }]];
            break;
        case dchat::ChatMessage::Kind::Notice:
        default:
            [line appendAttributedString:[[NSAttributedString alloc]
                                             initWithString:text
                                                 attributes:@{
                                                     NSForegroundColorAttributeName : DimColor()
                                                 }]];
            break;
    }

    [line appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"
                                                                attributes:dim]];
    [_transcript.textStorage appendAttributedString:line];
    [_transcript scrollRangeToVisible:NSMakeRange(_transcript.string.length, 0)];
}

- (void)appendSystemText:(NSString*)text {
    [self appendLineWithText:text
                        nick:@""
                        time:@""
                        kind:static_cast<int>(dchat::ChatMessage::Kind::System)
                     mention:NO
                    hasColor:NO
                      fileId:@""];
}

- (void)setStatus:(NSString*)text { _statusBar.stringValue = text; }
- (void)setMembers:(NSString*)text { _memberView.stringValue = text; }
- (void)setColorEnabled:(BOOL)enabled {
    _statusBar.stringValue = enabled ? @"彩色聊天：开" : @"彩色聊天：关（色码会原样显示）";
}
- (void)markLoggedIn:(NSString*)nick {
    _loggedIn = YES;
    _statusBar.stringValue =
        [NSString stringWithFormat:@"已登录：%@", nick.length ? nick : @"(未知)"];
    _chatWindow.title = [NSString stringWithFormat:@"dchat - %@", nick];
}

@end

// ---------------------------------------------------------------------------
// 应用启动
// ---------------------------------------------------------------------------
@interface DChatAppDelegateImpl : NSObject <NSApplicationDelegate>
@property(strong) DChatWindowController* controller;
@end

@implementation DChatAppDelegateImpl
- (void)applicationDidFinishLaunching:(NSNotification*)note {
    (void)note;
    self.controller = [[DChatWindowController alloc] init];
    [self.controller showConnectWindow];
    [NSApp activateIgnoringOtherApps:YES];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
    (void)app;
    return YES;
}
@end

int main(int argc, const char** argv) {
    (void)argc;
    (void)argv;
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        DChatAppDelegateImpl* delegate = [[DChatAppDelegateImpl alloc] init];
        NSApp.delegate = delegate;
        // 顶部菜单：只放一个"退出"，够用了
        NSMenu* menuBar = [[NSMenu alloc] init];
        NSMenuItem* appItem = [[NSMenuItem alloc] init];
        [menuBar addItem:appItem];
        NSMenu* appMenu = [[NSMenu alloc] init];
        [appMenu addItemWithTitle:@"退出 dchat"
                           action:@selector(terminate:)
                    keyEquivalent:@"q"];
        appItem.submenu = appMenu;
        NSApp.mainMenu = menuBar;
        [NSApp run];
    }
    return 0;
}
