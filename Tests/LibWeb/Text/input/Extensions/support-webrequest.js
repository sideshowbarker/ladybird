// Shared by the webRequest tests: a token for this page's requests, and the events the fixture's background page heard
// for the requests that carry it.
const webRequestToken = `wr-${crypto.randomUUID()}`;

async function watchWebRequests() {
    await contentScriptReady();
    return await sendToBackground({ type: "watch", token: webRequestToken });
}

// Waits until the background page has heard events that satisfy the predicate, and returns them all.
async function webRequestEvents(until) {
    for (;;) {
        const events = await sendToBackground({ type: "events", token: webRequestToken });
        if (until(events)) return events;
        await new Promise(resolve => setTimeout(resolve, 20));
    }
}

function echoPath(name) {
    return `/${webRequestToken}/${name}`;
}

function eventNames(events) {
    return events.map(event => event.event).join(" ");
}
