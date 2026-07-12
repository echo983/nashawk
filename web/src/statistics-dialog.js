/* @license This file Copyright © Mnemosyne LLC.
   It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
   or any future license endorsed by Mnemosyne LLC.
   License text can be found in the licenses/ folder. */

import { Formatter } from './formatter.js';
import {
  Utils,
  setTextContent,
  createDialogContainer,
  createInfoSection,
  isUsenetDebugEnabled,
} from './utils.js';

function getUsenetStats(stats) {
  return stats.usenet ?? (Object.hasOwn(stats, 'usenet_enabled') ? stats : {});
}

export class StatisticsDialog extends EventTarget {
  constructor(remote) {
    super();

    this.remote = remote;
    this.usenet_diagnostics = null;

    const updateDaemon = () =>
      this.remote.loadDaemonStats((data) => this._update(data.result));
    const delay_msec = 5000;
    this.interval = setInterval(updateDaemon, delay_msec);
    updateDaemon();

    this.elements = StatisticsDialog._create();
    this.elements.dismiss.addEventListener('click', () => this._onDismiss());
    this.elements.usenet.copy.addEventListener('click', () =>
      this._copyUsenetDiagnostics(),
    );
    document.body.append(this.elements.root);
    this.elements.dismiss.focus();
  }

  close() {
    if (!this.closed) {
      clearInterval(this.interval);
      this.elements.root.remove();
      this.dispatchEvent(new Event('close'));
      for (const key of Object.keys(this)) {
        delete this[key];
      }
      this.closed = true;
    }
  }

  _onDismiss() {
    this.close();
  }

  _copyUsenetDiagnostics() {
    const text = JSON.stringify(this.usenet_diagnostics ?? {}, null, 2);
    if (navigator.clipboard) {
      navigator.clipboard.writeText(text);
    } else {
      prompt('Select all then copy', text);
    }
  }

  _updateUsenet(stats) {
    const u = getUsenetStats(stats);
    const fmt = Formatter;
    const enabled = u.usenet_enabled === true;
    const known = 'usenet_enabled' in u;
    let mode = 'Unknown';
    let eviction = 'Unknown';
    if (known) {
      mode = enabled ? 'Enabled' : 'Disabled';
      eviction = u.usenet_eviction_enabled ? 'Enabled' : 'Disabled';
    }

    setTextContent(this.elements.usenet.mode, mode);
    setTextContent(
      this.elements.usenet.io,
      known
        ? `${fmt.number(u.usenet_io_active ?? 0)} / ${fmt.number(
            u.usenet_io_limit ?? 0,
          )}`
        : 'Unknown',
    );
    setTextContent(
      this.elements.usenet.upload_queue,
      known ? fmt.number(u.usenet_upload_queue_size ?? 0) : 'Unknown',
    );
    setTextContent(
      this.elements.usenet.download_queue,
      known
        ? `${fmt.number(u.usenet_download_queue_size ?? 0)} queued, ${fmt.number(
            u.usenet_download_in_flight ?? 0,
          )} in flight`
        : 'Unknown',
    );
    setTextContent(this.elements.usenet.eviction, eviction);
    setTextContent(
      this.elements.usenet.eviction_age,
      known
        ? fmt.countString(
            'minute',
            'minutes',
            u.usenet_eviction_min_age_minutes ?? 0,
          )
        : 'Unknown',
    );
    setTextContent(
      this.elements.usenet.cache,
      known ? fmt.mem((u.usenet_cache_size_mib ?? 0) * 1024 * 1024) : 'Unknown',
    );

    this.usenet_diagnostics = {
      generated_at: new Date().toISOString(),
      usenet: u,
    };

    if (isUsenetDebugEnabled()) {
      console.log(
        '[nashawk-usenet] session_stats.usenet',
        this.usenet_diagnostics,
      );
    }
  }

  _update(stats) {
    const fmt = Formatter;

    let s = stats.current_stats;
    let ratio = Utils.ratio(s.uploaded_bytes, s.downloaded_bytes);
    setTextContent(this.elements.session.up, fmt.size(s.uploaded_bytes));
    setTextContent(this.elements.session.down, fmt.size(s.downloaded_bytes));
    this.elements.session.ratio.innerHTML = fmt.ratioString(ratio);
    setTextContent(
      this.elements.session.time,
      fmt.timeInterval(s.seconds_active),
    );

    s = stats.cumulative_stats;
    ratio = Utils.ratio(s.uploaded_bytes, s.downloaded_bytes);
    setTextContent(this.elements.total.up, fmt.size(s.uploaded_bytes));
    setTextContent(this.elements.total.down, fmt.size(s.downloaded_bytes));
    this.elements.total.ratio.innerHTML = fmt.ratioString(ratio);
    setTextContent(
      this.elements.total.time,
      fmt.timeInterval(s.seconds_active),
    );

    this._updateUsenet(stats);
  }

  static _create() {
    const elements = createDialogContainer('statistics-dialog');
    const { confirm, dismiss, heading, root, workarea } = elements;
    confirm.remove();
    dismiss.textContent = 'Close';
    delete elements.confirm;

    const heading_text = 'Statistics';
    root.setAttribute('aria-label', heading_text);
    heading.textContent = heading_text;

    const labels = ['Uploaded:', 'Downloaded:', 'Ratio:', 'Running time:'];
    let section = createInfoSection('Current session', labels);
    const [sup, sdown, sratio, stime] = section.children;
    const session = (elements.session = {});
    session.up = sup;
    session.down = sdown;
    session.ratio = sratio;
    session.time = stime;
    workarea.append(section.root);

    section = createInfoSection('Total', labels);
    const [tup, tdown, tratio, ttime] = section.children;
    const total = (elements.total = {});
    total.up = tup;
    total.down = tdown;
    total.ratio = tratio;
    total.time = ttime;
    workarea.append(section.root);

    section = createInfoSection('Usenet', [
      'Mode:',
      'IO active:',
      'Upload queue:',
      'Download queue:',
      'Eviction:',
      'Eviction age:',
      'Cache target:',
    ]);
    const [
      usenet_mode,
      usenet_io,
      usenet_upload_queue,
      usenet_download_queue,
      usenet_eviction,
      usenet_eviction_age,
      usenet_cache,
    ] = section.children;
    const usenet = (elements.usenet = {});
    usenet.mode = usenet_mode;
    usenet.io = usenet_io;
    usenet.upload_queue = usenet_upload_queue;
    usenet.download_queue = usenet_download_queue;
    usenet.eviction = usenet_eviction;
    usenet.eviction_age = usenet_eviction_age;
    usenet.cache = usenet_cache;
    usenet.copy = document.createElement('button');
    usenet.copy.type = 'button';
    usenet.copy.textContent = 'Copy diagnostics';
    section.root.append(usenet.copy);
    workarea.append(section.root);

    return elements;
  }
}
