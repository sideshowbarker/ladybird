document.documentElement.dataset.storageOutput = JSON.stringify([String(browser.storage === undefined)]);
document.dispatchEvent(new Event("storage-finished"));
