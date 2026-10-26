// Answers each kind of message the way an extension's background page would.
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "ping")
        sendResponse({ answer: "pong", from: sender.url.endsWith("send-message-round-trip.html") });
    else if (message.type === "async") {
        setTimeout(() => sendResponse("async-pong"), 10);
        return true;
    } else if (message.type === "promise") return Promise.resolve("promise-pong");
    else if (message.type === "who")
        sendResponse({ id: browser.runtime.id === sender.id, background: location.pathname });
    else if (message.type === "helloFromBackground")
        browser.runtime
            .sendMessage({ type: "from-background" })
            .then(
                reply => `runtime: content said ${reply}`,
                error => `runtime rejected: ${error.message}`
            )
            .then(runtime =>
                browser.tabs.sendMessage(sender.tab.id, { type: "from-background" }).then(
                    reply => sendResponse(`${runtime}; tabs: content said ${reply}`),
                    error => sendResponse(`${runtime}; tabs rejected: ${error.message}`)
                )
            );
    if (message.type === "helloFromBackground") return true;
});
