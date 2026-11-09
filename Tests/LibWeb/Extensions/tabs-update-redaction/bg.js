// Opens a page whose title it may not see, sends that tab to one of its own pages, and reports what the update
// resolved with.
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type !== "run") return;
    (async () => {
        const tab = await browser.tabs.create({ url: "data:text/html,<title>Secret</title>" });
        const loaded = async () =>
            (await browser.tabs.query({})).find(found => found.id === tab.id)?.status === "complete";
        while (!(await loaded())) await new Promise(resolve => setTimeout(resolve, 20));
        const updated = await browser.tabs.update(tab.id, { url: `page.html?token=${message.token}` });
        // Its page answers the test once it has loaded, which shows in the URL the extension may see there.
        const showsItsPage = async () => {
            const found = (await browser.tabs.query({})).find(found => found.id === tab.id);
            return found?.status === "complete" && found.url?.includes("page.html");
        };
        while (!(await showsItsPage())) await new Promise(resolve => setTimeout(resolve, 20));
        sendResponse({ id: updated.id === tab.id, title: updated.title ?? null });
    })();
    return true;
});
