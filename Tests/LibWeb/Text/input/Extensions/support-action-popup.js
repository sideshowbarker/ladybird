// Shared by the popup tests: the fixture's id, its popup's state, and a way to wait for that state to change.
const popupFixture = () => internals.extensionId("Action Popup Fixture");

function popupState() {
    return JSON.parse(internals.extensionPopupState(popupFixture()));
}

async function waitFor(predicate) {
    for (let i = 0; i < 200; ++i) {
        const value = await predicate();
        if (value) return value;
        await new Promise(resolve => setTimeout(resolve, 25));
    }
    return null;
}

// The tests share a window, so a popup an earlier test left open is closed first, and each test closes its own.
async function closePopup() {
    if (!popupState().open) return;
    internals.clickExtensionAction(popupFixture());
    await waitFor(() => !popupState().open);
}

async function openPopup(popup) {
    await closePopup();
    await sendToBackground({ type: "set-popup", popup });
    internals.clickExtensionAction(popupFixture());
    await waitFor(() => sendToBackground({ type: "popup-report" }));
}
