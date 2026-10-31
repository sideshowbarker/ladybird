// Fetches the URL the test page names, and replies with the status, or the error the fetch failed with.
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    fetch(message.url).then(
        response => sendResponse({ status: response.status }),
        error => sendResponse({ failed: error.name })
    );
    return true;
});
