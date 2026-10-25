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
        // sender is a plain data property: a page may overwrite it, and uBO clears it once it has read it.
        const sender = port.sender;
        port.sender = undefined;
        const senderWritable = port.sender === undefined;
        port.sender = sender;
        port.postMessage({
            message,
            samePort: samePort === port,
            name: port.name,
            senderId: port.sender.id === browser.runtime.id,
            senderPage: port.sender.url.includes("/page.html?"),
            senderWritable,
        });
    });
});
browser.runtime.onMessage.addListener(message => {
    if (message === "last-disconnected") return Promise.resolve(globalThis.lastDisconnectedName);
});
