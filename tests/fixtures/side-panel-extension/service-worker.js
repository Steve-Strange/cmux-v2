chrome.runtime.onInstalled.addListener(async () => {
  await chrome.sidePanel.setPanelBehavior({ openPanelOnActionClick: true });
});

chrome.action.onClicked.addListener(async (tab) => {
  if (!tab.id) {
    return;
  }

  await chrome.sidePanel.setOptions({
    tabId: tab.id,
    path: "panel.html",
    enabled: true,
  });
  await chrome.sidePanel.open({ tabId: tab.id });
});
