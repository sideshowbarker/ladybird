// What a fetch() gave its caller: the status, body, and a header, or the error it failed with.
async function fetchResult(url, init) {
    try {
        const response = await fetch(url, init);
        return { status: response.status, body: await response.text(), custom: response.headers.get("X-Custom") };
    } catch (error) {
        return { failed: error.name };
    }
}
