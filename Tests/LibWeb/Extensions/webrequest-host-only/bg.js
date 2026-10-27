// Records the URLs of the requests this extension hears that carry a token a test page is watching.
const heard = new Map();
browser.webRequest.onBeforeRequest.addListener(
    details => {
        for (const [token, urls] of heard) {
            if (details.url.includes(token)) urls.push(details.url);
        }
    },
    { urls: ["<all_urls>"] }
);
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "watch") heard.set(message.token, []);
    if (message.type === "heard") sendResponse(heard.get(message.token));
    else sendResponse(true);
});
