function showLoading() {
  var overlay = document.getElementById('loading-overlay');
  if (overlay) overlay.classList.remove('hidden');
  setButtonsDisabled(true);
}

function hideLoading() {
  var overlay = document.getElementById('loading-overlay');
  if (overlay) overlay.classList.add('hidden');
  setButtonsDisabled(false);
}

function setButtonsDisabled(disabled) {
  var btns = document.querySelectorAll('.button-grid button, .relay-grid button, .controls button, .settings-button, .tool-button');
  btns.forEach(function (b) {
    b.disabled = disabled;
    b.style.opacity = disabled ? 0.6 : 1;
    b.style.pointerEvents = disabled ? 'none' : 'auto';
  });
}

function toggleRelay(id) {
  showLoading();
  var button = document.getElementById('relay' + id);
  if (button) {
    if (button.classList.contains('green')) {
      button.classList.remove('green');
      button.classList.add('red');
    } else if (button.classList.contains('red')) {
      button.classList.remove('red');
      button.classList.add('green');
    }
  }
  fetch('/toggle?id=' + id)
    .catch(function () { })
    .finally(function () {
      setTimeout(function () {
        updateStatus().finally(hideLoading);
      }, 500);
    });
}

function allRelaysOn() {
  showLoading();
  fetch('/all_on')
    .catch(function () { })
    .finally(function () {
      setTimeout(function () {
        updateStatus().finally(hideLoading);
      }, 1000);
    });
}

function allRelaysOff() {
  showLoading();
  fetch('/all_off')
    .catch(function () { })
    .finally(function () {
      setTimeout(function () {
        updateStatus().finally(hideLoading);
      }, 1000);
    });
}

function updateStatus() {
  return fetch('/api/status')
    .then(function (response) { return response.json(); })
    .then(function (data) {
      var shouldBeInactive = (data.remoteSleeping === true || data.relayDataReady === false);

      for (var i = 0; i < data.states.length; i++) {
        var button = document.getElementById('relay' + i);
        if (!button) continue;

        button.classList.remove('green', 'red', 'active', 'grey', 'inactive');

        if (shouldBeInactive) {
          button.classList.add('grey', 'inactive');
        } else if (data.states[i] === true || data.states[i] === 1) {
          button.classList.add('green', 'active');
        } else {
          button.classList.add('red', 'active');
        }

        var label = data.labels && data.labels[i] ? data.labels[i] : button.innerText;
        button.innerText = label;
      }

      // Status message
      var statusDiv = document.getElementById('status-message');
      if (statusDiv && data.lastStatus !== undefined) {
        if (data.lastStatus && data.lastStatus.length > 0) {
          statusDiv.innerHTML = "<p style='margin:5px;'>" + data.lastStatus + "</p>";
        } else {
          statusDiv.innerHTML = "<p style='margin:5px; color:#888;'>No status messages</p>";
        }
      }

      // Signal pill
      var signalPill = document.getElementById('signal-pill');
      if (signalPill && data.lora) {
        if (data.lora.valid) {
          var rssiText = (typeof data.lora.rssi === 'number') ? data.lora.rssi.toFixed(0) + ' dBm' : '--';
          var snrText = (typeof data.lora.snr === 'number') ? data.lora.snr.toFixed(1) + ' dB' : '--';
          signalPill.textContent = 'Signal ' + rssiText + ' / ' + snrText;
        } else {
          signalPill.textContent = 'Signal --';
        }
      }

      // Temperature footer
      if (data.temperatures && data.temperatures.length > 0) {
        if (data.temperatures[0]) {
          var temp1Span = document.getElementById('temp1');
          var temp1Container = document.getElementById('footer-temp1');
          if (temp1Span && temp1Container) {
            temp1Span.textContent = (data.temperatures[0].label || 'Temp') + ': ' + data.temperatures[0].celsius.toFixed(1) + '\u00b0C';
            temp1Container.style.display = '';
          }
        }
        if (data.temperatures[1]) {
          var temp2Span = document.getElementById('temp2');
          var temp2Container = document.getElementById('footer-temp2');
          if (temp2Span && temp2Container) {
            temp2Span.textContent = (data.temperatures[1].label || 'Temp') + ': ' + data.temperatures[1].celsius.toFixed(1) + '\u00b0C';
            temp2Container.style.display = '';
          }
        }
      }
    })
    .catch(function () { });
}

function clearLog() {
  if (confirm('Clear the log?')) {
    fetch('/clearlog').then(function (r) {
      if (r.ok) location.reload();
      else alert('Failed to clear log.');
    });
  }
}

document.addEventListener('DOMContentLoaded', function () {
  updateStatus();
  if (window.location.pathname === '/' || window.location.pathname === '') {
    setInterval(updateStatus, 3000);
  }
  if (window.location.pathname === '/log') {
    setTimeout(function () { window.location.reload(); }, 15000);
    var logBox = document.querySelector('.log-box');
    if (logBox) logBox.scrollTop = logBox.scrollHeight;
  }
});
