// Injects into every frame of the tab that asks, immediately and without pause, until that tab says stop: a script
// element each time, which the frame's page runs. Then reports how many injections failed and whether the frame the
// page loaded meanwhile ran any of them.
let running = false;
let failed = 0;
const appendScript = () => {
    const script = document.createElement("script");
    script.textContent = "window.injectedScripts = (window.injectedScripts ?? 0) + 1;";
    document.documentElement.appendChild(script);
    script.remove();
};
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const target = { tabId: sender.tab.id, allFrames: true };
    if (message.type === "start") {
        running = true;
        failed = 0;
        (async () => {
            while (running) {
                try {
                    await browser.scripting.executeScript({ target, func: appendScript, injectImmediately: true });
                } catch (error) {
                    ++failed;
                }
            }
        })();
        sendResponse("started");
        return;
    }
    if (message.type === "stop") {
        running = false;
        browser.scripting
            .executeScript({ target, func: () => [location.search, window.injectedScripts ?? 0], world: "MAIN" })
            .then(results => {
                const frame = results.map(result => result.result).find(([search]) => search === "?target");
                sendResponse({ failed, frameRan: frame ? frame[1] : -1 });
            });
        return true;
    }
});
