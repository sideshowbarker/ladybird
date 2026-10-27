// Changes the page as Refined GitHub's features do, by its options from storage.sync, and follows their changes; it also
// relays the test page's requests to the background page.
const apply = accent => document.documentElement.style.setProperty("--model-accent", accent);
browser.storage.sync.get({ accent: "rgb(10, 20, 30)" }).then(({ accent }) => apply(accent));
browser.storage.onChanged.addListener((changes, area) => {
    if (area === "sync" && changes.accent) apply(changes.accent.newValue);
});
document.addEventListener("DOMContentLoaded", () => {
    const badge = document.createElement("span");
    badge.id = "model-badge";
    badge.textContent = "refined";
    document.body.prepend(badge);
});
document.addEventListener("tabs-request", async event => {
    let reply;
    try {
        reply = await browser.runtime.sendMessage(JSON.parse(event.detail));
    } catch (error) {
        reply = { failed: `${error.name}: ${error.message}` };
    }
    document.documentElement.dataset.tabsReply = JSON.stringify(reply);
    document.dispatchEvent(new CustomEvent("tabs-reply"));
});
document.documentElement.dataset.contentScriptReady = "1";
document.dispatchEvent(new CustomEvent("content-script-ready"));
