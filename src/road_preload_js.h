#pragma once
// Served as JavaScript; tests extract this same source and compare its output
// with the portable C++ generator, including negative-coordinate rounding.
static const char ROAD_PRELOAD_JS[] PROGMEM = R"JS(
'use strict';
globalThis.RoadTiles = (() => {
  const round = x => x < 0 ? -Math.round(-x) : Math.round(x);
  function simplify(points) {
    if (points.length < 2) return [];
    const keep = new Uint8Array(points.length), stack = [0, points.length-1];
    keep[0] = keep[points.length-1] = 1;
    while (stack.length) {
      const hi = stack.pop(), lo = stack.pop(), a = points[lo], b = points[hi];
      const dx = b.lon-a.lon, dy = b.lat-a.lat, length2 = dx*dx+dy*dy;
      let best = .0025*.0025, index = 0;
      for (let i = lo+1; i < hi; ++i) {
        const p = points[i];
        let t = length2 ? ((p.lon-a.lon)*dx+(p.lat-a.lat)*dy)/length2 : 0;
        t = Math.max(0, Math.min(1,t));
        const ex = p.lon-a.lon-t*dx, ey = p.lat-a.lat-t*dy;
        if (ex*ex+ey*ey > best) { best = ex*ex+ey*ey; index = i; }
      }
      if (index) { keep[index] = 1; stack.push(lo,index,index,hi); }
    }
    const result = [];
    for (let i = 0; i < points.length; ++i) if (keep[i]) {
      const y = round(points[i].lat*180), x = round(points[i].lon*180), last = result.at(-1);
      if (!last || last[0] !== y || last[1] !== x) result.push([y,x]);
    }
    return result.length >= 2 ? result : [];
  }
  function clip(points, cell) {
    const lines = [], south = cell.lat*45, west = cell.lon*45, width = cell.span*45;
    let run = [];
    const flush = () => { if (run.length >= 2) lines.push(run); run = []; };
    for (let i = 1; i < points.length; ++i) {
      const [y,x] = points[i-1], dy = points[i][0]-y, dx = points[i][1]-x;
      let lo = 0, hi = 1;
      const edge = (a,b) => {
        if (a === 0) return b >= 0;
        const t = b/a;
        if (a < 0) { if (t > hi) return false; if (t > lo) lo = t; }
        else { if (t < lo) return false; if (t < hi) hi = t; }
        return true;
      };
      if (!edge(-dx,x-west) || !edge(dx,west+width-x) || !edge(-dy,y-south) || !edge(dy,south+width-y)) { flush(); continue; }
      const a = [round(y+lo*dy),round(x+lo*dx)], b = [round(y+hi*dy),round(x+hi*dx)];
      if (a[0] === b[0] && a[1] === b[1]) continue;
      const last = run.at(-1);
      if (last && (last[0] !== a[0] || last[1] !== a[1] || run.length === 256)) flush();
      if (!run.length) run.push(a);
      run.push(b);
      if (hi < 1) flush();
    }
    flush(); return lines;
  }
  async function process(doc, cell) {
    if (Object.prototype.hasOwnProperty.call(doc,'remark')) throw new Error('Map service: '+String(doc.remark).slice(0,240));
    if (!Array.isArray(doc.elements)) throw new Error('Map service returned no geometry array');
    const lines = []; let ways = 0, pointCount = 0;
    for (let i = 0; i < doc.elements.length; ++i) {
      const way = doc.elements[i];
      if (way.type !== 'way' || !Array.isArray(way.geometry) || way.geometry.length > 2048) throw new Error('Invalid or oversized road geometry');
      for (const p of way.geometry) if (!p || typeof p.lat !== 'number' || typeof p.lon !== 'number' || !Number.isFinite(p.lat) || !Number.isFinite(p.lon) || Math.abs(p.lat)>90 || Math.abs(p.lon)>180) throw new Error('Invalid road coordinate');
      const points = simplify(way.geometry);
      if (points.length && ++ways > 16000) throw new Error('Too many roads in this tile');
      for (const line of clip(points,cell)) {
        lines.push(line); pointCount += line.length;
        if (lines.length > 16000 || 6+lines.length*2+pointCount*4 > 512*1024) throw new Error('Tile exceeds upload limit');
      }
      if (i % 32 === 31) await new Promise(resolve => setTimeout(resolve,0));
    }
    const buffer = new ArrayBuffer(6+lines.length*2+pointCount*4), view = new DataView(buffer);
    view.setUint16(0,lines.length,true); view.setUint32(2,pointCount,true);
    let at = 6;
    for (const line of lines) { view.setUint16(at,line.length,true); at += 2; }
    for (const line of lines) for (const [y,x] of line) {
      view.setInt16(at,y,true); view.setInt16(at+2,x,true); at += 4;
    }
    return buffer;
  }
  function query(cell) {
    if (!['motorway','trunk'].includes(cell.roadClass)) throw new Error('Invalid road class');
    const bounds = [cell.lat/4,cell.lon/4,(cell.lat+cell.span)/4,(cell.lon+cell.span)/4];
    return '[out:json][timeout:45][maxsize:134217728];way[highway="'+cell.roadClass+'"]('+bounds.join(',')+');out skel geom;';
  }
  return {process, query};
})();
)JS";
