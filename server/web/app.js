/* SPDX-License-Identifier: GPL-3.0-only */
const $ = id => document.getElementById(id);
const text = (id, value) => { $(id).textContent = value; };
async function api(path, method='GET') {
  const result = await fetch(path, {method, headers: method === 'POST' ? {'Content-Type':'application/json'} : {}});
  const body = await result.json();
  if (!result.ok) throw new Error(body.error || `HTTP ${result.status}`);
  return body;
}
function message(m) { text('message', m); }
async function update() {
  try {
    const data = await api('/admin/api/state');
    text('ts-state', data.tailscale.state || 'Unavailable');
    text('ts-host', data.tailscale.hostname || '—');
    text('ts-ip', (data.tailscale.ips || []).join(', ') || '—');
    text('uploads', data.active_uploads);
    const login = $('login-url'); login.replaceChildren();
    if (data.login_url) {
      const a=document.createElement('a');a.href=data.login_url;a.target='_blank';a.rel='noopener noreferrer';a.textContent='OPEN TAILSCALE LOGIN';login.append(a);
    } else if (data.login_busy) login.textContent='Waiting for login URL…';
    const body=$('devices');body.replaceChildren();
    const list=(data.devices || []).sort((a,b) => a.created.localeCompare(b.created));
    if (!list.length) { const row=body.insertRow();const cell=row.insertCell();cell.colSpan=4;cell.textContent='No Desktop requests'; }
    for (const item of list) {
      const row=body.insertRow(); const device=row.insertCell();
      device.textContent=item.name+' ';const code=document.createElement('code');code.textContent=item.id.substring(0,12);device.append(code);
      row.insertCell().textContent=new Date(item.created).toLocaleString();
      row.insertCell().textContent=item.state.toUpperCase();
      const action=row.insertCell();
      const b=document.createElement('button');
      if (item.state==='pending'){b.textContent='APPROVE';b.onclick=()=>modify(item.id,'approve');}
      else if(item.state==='approved'){b.textContent='REVOKE';b.className='outline';b.onclick=()=>modify(item.id,'revoke');}
      else {b.textContent='REVOKED';b.disabled=true;}
      action.append(b);
    }
  } catch(e) { message(`Status error: ${e.message}`); }
}
async function modify(id, action) {
  if(action==='revoke' && !confirm('Revoke this Desktop? Ongoing and future uploads will be denied.'))return;
  try {await api(`/admin/api/${action}/${id}`,'POST');message(`${action} successful`);await update();}
  catch(e){message(e.message);}
}
$('refresh').onclick=update;
$('login').onclick=async()=>{try{await api('/admin/api/tailscale/login','POST');message('Login requested. Open the link when it appears.');await update();}catch(e){message(e.message);}};
$('logout').onclick=async()=>{if(!confirm('Log out the MediaBridge Tailscale node? Uploads will stop.'))return;try{await api('/admin/api/tailscale/logout','POST');await update();}catch(e){message(e.message);}};
update();setInterval(update,5000);
