// Calls the action API on the page's behalf, and answers with what it resolved or how it failed. A "$tab" argument
// stands for the id of the tab that asked.
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const api = browser.action ?? browser.browserAction;
    if (message.type === "namespaces") {
        sendResponse({
            action: typeof browser.action,
            browserAction: typeof browser.browserAction,
            setBadgeTextColor: typeof api.setBadgeTextColor,
        });
        return;
    }
    if (message.type === "child") {
        handleChild(message.run, message.step, sender.tab.id).then(sendResponse);
        return true;
    }
    if (message.type === "send-to-tab") {
        browser.tabs.sendMessage(sender.tab.id, "hello").then(
            () => sendResponse("delivered"),
            error => sendResponse(`${error.name}: ${error.message}`)
        );
        return true;
    }
    if (message.type === "child-state") {
        sendResponse(childStates.get(message.run) ?? {});
        return;
    }
    if (message.type === "throwing-color") {
        const color = [0, 0, 0, 255];
        Object.defineProperty(color, 0, {
            get() {
                throw new Error("the getter threw");
            },
        });
        try {
            api.setBadgeBackgroundColor({ color });
            sendResponse("no exception");
        } catch (error) {
            sendResponse(`${error.name}: ${error.message}`);
        }
        return;
    }
    const args = JSON.parse(JSON.stringify(message.args).replaceAll('"$tab"', String(sender.tab.id)));
    Promise.resolve()
        .then(() => api[message.method](...args))
        .then(
            value => sendResponse({ value: value ?? null }),
            error => sendResponse({ error: `${error.name}: ${error.message}` })
        );
    return true;
});

// A child window that a test opens reports each step under the test's run token; its tab's badge is set at the first
// step and read at the others.
const childStates = new Map();
async function handleChild(run, step, tabId) {
    if (!childStates.has(run)) childStates.set(run, {});
    const childState = childStates.get(run);
    if (step === "loaded") {
        await browser.action.setBadgeText({ tabId, text: "child" });
        childState.tabId = tabId;
    } else {
        // Other tests may have set a global badge, which a tab without one of its own shows.
        const badge = await browser.action.getBadgeText({ tabId: childState.tabId });
        const global = await browser.action.getBadgeText({});
        childState[step] = {
            sameTab: tabId === childState.tabId,
            badge: badge === global ? "the global value" : badge,
        };
    }
    return true;
}
