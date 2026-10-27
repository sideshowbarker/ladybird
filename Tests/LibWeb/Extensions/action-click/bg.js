// Records each click on the button, with the tab it came from, and answers the test page's questions.
const clicks = [];
browser.action.onClicked.addListener(tab => clicks.push({ sameTab: tab.id === lastAskingTab, url: tab.url ?? null }));
let lastAskingTab;
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    lastAskingTab = sender.tab.id;
    if (message.type === "clicks") {
        sendResponse(clicks.splice(0));
    } else if (message.type === "url") {
        browser.tabs.query({}).then(tabs => {
            const tab = tabs.find(tab => tab.id === sender.tab.id);
            sendResponse(tab.url ?? null);
        });
        return true;
    } else if (message.type === "set-popup") {
        browser.action.setPopup({ popup: message.popup }).then(() => sendResponse(true));
        return true;
    }
});
