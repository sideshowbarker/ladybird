browser.runtime.onConnect.addListener(port => {
    if (port.name === "port-disconnect-in-onconnect") {
        port.disconnect();
        return;
    }
    if (port.name === "port-two-receivers") {
        port.onMessage.addListener(message => {
            if (message === "close-first") {
                port.disconnect();
            }
        });
        port.postMessage("first-ready");
        return;
    }
    if (port.name === "port-disconnect-semantics") {
        port.onDisconnect.addListener(disconnected => {
            globalThis.lastDisconnectedName = disconnected.name;
        });
    }
    port.onMessage.addListener((message, samePort) => {
        port.postMessage({
            message,
            samePort: samePort === port,
            name: port.name,
            senderId: port.sender.id === browser.runtime.id,
            senderPage: port.sender.url.includes("/page.html?"),
        });
    });
});
browser.runtime.onMessage.addListener(message => {
    if (message === "last-disconnected") return Promise.resolve(globalThis.lastDisconnectedName);
});
