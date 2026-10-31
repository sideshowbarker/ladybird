// Stores large values in storage.local, and works with an IndexedDB database, as the test page asks.
const value = (index, length) => String.fromCharCode(65 + index).repeat(length);
const checksum = text => {
    let sum = 0;
    for (let i = 0; i < text.length; i += 4096) sum = (sum * 31 + text.charCodeAt(i) + i) % 1000003;
    return sum;
};
const request = idbRequest =>
    new Promise((resolve, reject) => {
        idbRequest.onsuccess = () => resolve(idbRequest.result);
        idbRequest.onerror = () => reject(idbRequest.error);
    });

async function largeValues(megabytes) {
    const length = megabytes * 1024 * 1024;
    const items = { a: value(0, length), b: value(1, length), c: value(2, length) };
    await browser.storage.local.set(items);
    const stored = await browser.storage.local.get(["a", "b", "c"]);
    const intact = Object.keys(items).every(
        key => stored[key].length === length && checksum(stored[key]) === checksum(items[key])
    );
    const bytes = await browser.storage.local.getBytesInUse(null);
    await browser.storage.local.remove(["a", "b", "c"]);
    return { intact, atLeast: bytes >= 3 * length };
}

async function indexedDBRoundTrip() {
    const name = `storage-large-${Math.random()}`;
    const open = indexedDB.open(name, 1);
    open.onupgradeneeded = () => open.result.createObjectStore("cache");
    const db = await request(open);
    const write = db.transaction("cache", "readwrite").objectStore("cache");
    await request(write.put(value(3, 1024 * 1024), "big"));
    await request(write.put("one", "first"));
    await request(write.put("two", "second"));
    const read = db.transaction("cache", "readonly").objectStore("cache");
    const big = await request(read.get("big"));
    const keys = [];
    await new Promise((resolve, reject) => {
        const cursor = read.openCursor();
        cursor.onsuccess = () => {
            if (!cursor.result) return resolve();
            keys.push(cursor.result.key);
            cursor.result.continue();
        };
        cursor.onerror = () => reject(cursor.error);
    });
    await request(db.transaction("cache", "readwrite").objectStore("cache").delete("first"));
    const remaining = await request(db.transaction("cache", "readonly").objectStore("cache").count());
    db.close();
    await request(indexedDB.deleteDatabase(name));
    return { bigLength: big.length, keys, remaining };
}

browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const task = message.type === "large" ? largeValues(message.megabytes) : indexedDBRoundTrip();
    task.then(sendResponse, error => sendResponse({ failed: `${error.name}: ${error.message}` }));
    return true;
});
