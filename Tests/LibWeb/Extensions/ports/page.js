(async () => {
    const mode = location.search.slice(1);
    const output = [];
    const log = value => output.push(String(value));
    const next = port =>
        new Promise(resolve =>
            port.onMessage.addListener(function listener(message, samePort) {
                port.onMessage.removeListener(listener);
                resolve([message, samePort]);
            })
        );
    const disconnected = port => new Promise(resolve => port.onDisconnect.addListener(resolve));
    try {
        if (typeof browser.runtime.connect !== "function") throw new TypeError("runtime.connect is missing");
        if (mode === "port-two-receivers") {
            const loaded = new Promise(resolve =>
                window.addEventListener("message", event => {
                    if (event.data === "second-loaded") resolve();
                })
            );
            const frame = document.createElement("iframe");
            frame.src = "second.html";
            document.body.append(frame);
            await loaded;
            const port = browser.runtime.connect({ name: mode });
            let disconnects = 0;
            port.onDisconnect.addListener(() => ++disconnects);
            const ready = new Set();
            await new Promise(resolve =>
                port.onMessage.addListener(message => {
                    ready.add(message);
                    if (ready.has("first-ready") && ready.has("second-ready")) resolve();
                })
            );
            log("both ready");
            port.postMessage("close-first");
            const reply = next(port);
            port.postMessage("first-left");
            log((await reply)[0]);
            log(`still connected: ${disconnects === 0}`);
            const closed = disconnected(port);
            port.postMessage("close-second");
            await closed;
            log(`last receiver: ${disconnects === 1}, clean: ${port.error === null}`);
            frame.remove();
        } else if (mode === "port-no-receiver") {
            const port = browser.runtime.connect("another-extension", { name: "absent" });
            log(`synchronous: ${port.name}`);
            await new Promise(resolve =>
                port.onDisconnect.addListener(samePort => {
                    log(`identity: ${samePort === port}`);
                    log(port.error.message);
                    log(browser.runtime.lastError.message);
                    resolve();
                })
            );
            log(`after: ${browser.runtime.lastError}`);
        } else if (mode === "port-disconnect-in-onconnect") {
            const port = browser.runtime.connect({ name: mode });
            let messages = 0;
            port.onMessage.addListener(() => ++messages);
            const closed = disconnected(port);
            port.postMessage("queued");
            await closed;
            log(`clean: ${port.error === null}, messages: ${messages}`);
        } else if (mode === "port-disconnect-semantics") {
            const port = browser.runtime.connect({ name: mode });
            const ready = next(port);
            port.postMessage("ready");
            await ready;
            let events = 0;
            port.onDisconnect.addListener(() => ++events);
            port.disconnect();
            port.disconnect();
            try {
                port.postMessage(1);
            } catch (error) {
                log(`${error.constructor.name}: ${error.message}`);
            }
            log(`peer notified: ${(await browser.runtime.sendMessage("last-disconnected")) === mode}`);
            log(`local events: ${events}, clean: ${port.error === null}`);
        } else {
            const port = browser.runtime.connect(browser.runtime.id, { name: mode });
            if (mode === "port-early-messages") {
                const messages = [];
                const complete = new Promise(resolve =>
                    port.onMessage.addListener(reply => {
                        messages.push(reply.message);
                        if (messages.length === 3) resolve();
                    })
                );
                port.postMessage(1);
                port.postMessage(2);
                port.postMessage(3);
                await complete;
                log(JSON.stringify(messages));
            } else if (mode === "port-unserializable") {
                const cyclic = {};
                cyclic.self = cyclic;
                for (const value of [cyclic, 1n, undefined, () => 1]) {
                    try {
                        port.postMessage(value);
                        log("unexpected success");
                    } catch (error) {
                        log(error instanceof TypeError);
                    }
                }
                const reply = next(port);
                port.postMessage("still usable");
                log((await reply)[0].message);
            } else {
                log(`name: ${port.name}, sender absent: ${!("sender" in port)}, error: ${port.error}`);
                for (const value of [1, { text: "two" }, [3]]) {
                    const reply = next(port);
                    port.postMessage(value);
                    const [message, samePort] = await reply;
                    log(
                        `${JSON.stringify(message.message)}: ${samePort === port && message.samePort && message.senderId && message.senderPage && message.name === mode}`
                    );
                }
            }
            port.disconnect();
        }
    } catch (error) {
        log(`ERROR: ${error.name}: ${error.message}`);
    }
    parent.postMessage({ portTestOutput: output }, "*");
})();
