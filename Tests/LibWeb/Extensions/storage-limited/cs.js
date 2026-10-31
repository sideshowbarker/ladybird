// Relays the test page's request to the background page, with events of this fixture's own.
document.addEventListener("limited-request", async () => {
    const reply = await browser.runtime.sendMessage({});
    document.documentElement.dataset.limitedReply = JSON.stringify(reply);
    document.dispatchEvent(new CustomEvent("limited-reply"));
});
