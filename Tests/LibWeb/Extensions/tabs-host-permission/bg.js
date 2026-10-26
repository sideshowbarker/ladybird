// Runs the tabs.query() each message asks for, with "$self" standing for the URL of the page that asked.
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const query = JSON.parse(JSON.stringify(message.query).replaceAll('"$self"', JSON.stringify(sender.url)));
    let call;
    try {
        call = browser.tabs.query(query);
    } catch (error) {
        sendResponse({ thrown: `${error.name}: ${error.message}` });
        return;
    }
    call.then(
        tabs => sendResponse({ tabs }),
        error => sendResponse({ rejected: error.message })
    );
    return true;
});
