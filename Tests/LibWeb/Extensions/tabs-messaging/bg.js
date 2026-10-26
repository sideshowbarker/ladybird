// Messages the tab that asked, or one of its frames, with tabs.sendMessage(), and reports what came back.
let subframeId = null;
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "register") {
        subframeId = sender.frameId;
        sendResponse(true);
        return;
    }
    if (message.type === "describe-sender") {
        sendResponse({
            tabId: typeof sender.tab?.id,
            tabIndex: sender.tab?.index,
            tabUrl: "url" in (sender.tab ?? {}),
            frameId: sender.frameId,
            subframeId: subframeId === null ? null : subframeId > 0,
        });
        return;
    }
    const tabId = { missing: 987654321, huge: 1e100 }[message.tab] ?? sender.tab.id;
    const options = {
        top: { frameId: 0 },
        subframe: { frameId: subframeId },
        missing: { frameId: 424242 },
        negative: { frameId: -1 },
    }[message.frame];
    if (message.type === "ask") {
        let call;
        try {
            call = options
                ? browser.tabs.sendMessage(tabId, { type: "who" }, options)
                : browser.tabs.sendMessage(tabId, { type: "who" });
        } catch (error) {
            sendResponse({ thrown: `${error.name}: ${error.message}` });
            return;
        }
        call.then(
            reply => sendResponse({ reply }),
            error => sendResponse({ rejected: error.message })
        );
        return true;
    }
    if (message.type === "ask-with-callback") {
        chrome.tabs.sendMessage(tabId, { type: "who" }, options ?? {}, reply =>
            sendResponse({ reply, lastError: chrome.runtime.lastError?.message })
        );
        return true;
    }
});
