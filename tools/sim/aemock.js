// Faux After Effects (API ExtendScript minimale et STRICTE) pour exécuter les scripts d'Omnis.
const fs = require('fs'), vm = require('vm');
const PVT = {OneD: 1, TwoD: 2, ThreeD: 3, TwoD_SPATIAL: 4, ThreeD_SPATIAL: 5, COLOR: 6, TEXT_DOCUMENT: 7, NO_VALUE: 8, CUSTOM_VALUE: 9};
function KeyframeEase(speed, influence) { if (!(this instanceof KeyframeEase)) throw new Error('KeyframeEase sans new'); if (influence < 0.1 || influence > 100) throw new Error('influence hors 0.1-100'); this.speed = speed; this.influence = influence; }
class TextDocument { constructor(t) { this.text = t; this.fontSize = 72; this.fillColor = [1, 1, 1]; this.font = 'Arial'; this.justification = 0; this.applyFill = true; this.tracking = 0; } }
class Property {
  constructor(name, type, value) { this.name = name; this.matchName = name; this.propertyValueType = type; this.value = value; this.keys = []; this.expression = ''; this.expressionEnabled = true; }
  get numKeys() { return this.keys.length; }
  dims() { return Array.isArray(this.value) ? this.value.length : 1; }
  check(v) { if (this.propertyValueType === PVT.TEXT_DOCUMENT) { if (!(v instanceof TextDocument) && typeof v !== 'string') throw new Error(this.name + ' : TextDocument attendu'); return; }
    if (Array.isArray(this.value) !== Array.isArray(v)) throw new Error(this.name + ' : valeur de mauvaise forme ' + JSON.stringify(v));
    if (Array.isArray(v) && v.length < 2) throw new Error(this.name + ' : pas assez de dimensions'); if (!Array.isArray(v) && typeof v !== 'number') throw new Error(this.name + ' : nombre attendu'); }
  setValue(v) { this.check(v); if (this.keys.length) throw new Error('setValue sur une propriété avec clés'); this.value = v; }
  setValueAtTime(t, v) { if (typeof t !== 'number') throw new Error('temps non numérique'); this.check(v); this.keys = this.keys.filter(k => k.t !== t); this.keys.push({t, v}); this.keys.sort((a, b) => a.t - b.t); }
  addKey(t) { this.setValueAtTime(t, this.value); return this.keys.findIndex(k => k.t === t) + 1; }
  keyTime(i) { if (i < 1 || i > this.keys.length) throw new Error('keyTime : index ' + i + ' hors limites'); return this.keys[i - 1].t; }
  keyValue(i) { if (i < 1 || i > this.keys.length) throw new Error('keyValue : index hors limites'); return this.keys[i - 1].v; }
  valueAtTime(t) { return this.keys.length ? this.keys[0].v : this.value; }
  nearestKeyIndex(t) { if (!this.keys.length) throw new Error('aucune clé'); return 1; }
  setTemporalEaseAtKey(i, a, b) { this.keyTime(i); const n = [PVT.TwoD_SPATIAL, PVT.ThreeD_SPATIAL, PVT.OneD].includes(this.propertyValueType) ? 1 : this.dims();
    if (!Array.isArray(a) || a.length !== n) throw new Error(this.name + ' : setTemporalEaseAtKey attend ' + n + ' KeyframeEase'); }
  setInterpolationTypeAtKey(i) { this.keyTime(i); }
  setSpatialTangentsAtKey(i) { this.keyTime(i); }
  property(x) { return this.props ? this.props.get(x) : undefined; }
}
class Group { constructor(name, list) { this.name = name; this.matchName = name; this.list = list; } get numProperties() { return this.list.length; }
  property(x) { if (typeof x === 'number') return this.list[x - 1]; return this.list.find(p => p.name === x || p.matchName === x || (p.alias || []).includes(x)); }
  addProperty(mn) { const known = {'ADBE Gaussian Blur 2': ['Blurriness', 'Blur Dimensions', 'Repeat Edge Pixels'], 'ADBE Drop Shadow': ['Shadow Color', 'Opacity', 'Direction', 'Distance', 'Softness'], 'ADBE Glo2': ['Glow Based On', 'Glow Threshold', 'Glow Radius', 'Glow Intensity'], 'ADBE Fill': ['Fill Mask', 'All Masks', 'Color'], 'ADBE Tint': ['Map Black To', 'Map White To', 'Amount to Tint'], 'ADBE Geometry2': ['Anchor Point', 'Position', 'Uniform Scale', 'Scale', 'Scale Width', 'Skew', 'Skew Axis', 'Rotation', 'Opacity']};
    if (!known[mn]) throw new Error('effet inconnu : ' + mn); const g = new Group(mn, known[mn].map(n => new Property(n, /Color|Map/.test(n) ? PVT.COLOR : PVT.OneD, /Color|Map/.test(n) ? [0, 0, 0, 1] : 0))); this.list.push(g); return g; } }
let nLayers = 0;
class Layer { constructor(comp, name, text) { this.comp = comp; this.name = name; this.index = 0; this.inPoint = 0; this.outPoint = comp.duration; this.startTime = 0; this.motionBlur = false; this.parent = null; this.selected = false; this.enabled = true; this.threeDLayer = false;
    const mk = (n, t, v, al) => { const p = new Property(n, t, v); p.alias = al; return p; };
    this.transform = new Group('ADBE Transform Group', [mk('Anchor Point', PVT.ThreeD_SPATIAL, [0, 0, 0], ['ADBE Anchor Point', 'anchorPoint']), mk('Position', PVT.ThreeD_SPATIAL, [comp.width / 2, comp.height / 2, 0], ['ADBE Position', 'position']),
      mk('Scale', PVT.ThreeD, [100, 100, 100], ['ADBE Scale', 'scale']), mk('Rotation', PVT.OneD, 0, ['ADBE Rotate Z', 'rotation', 'Z Rotation']), mk('Opacity', PVT.OneD, 100, ['ADBE Opacity', 'opacity'])]);
    this.transform.alias = ['Transform']; for (const p of this.transform.list) this.transform[p.alias[1]] = p;
    this.effects = new Group('ADBE Effect Parade', []); this.effects.alias = ['Effects'];
    const groups = [this.transform, this.effects];
    if (text !== undefined) { const st = mk('Source Text', PVT.TEXT_DOCUMENT, new TextDocument(text), ['ADBE Text Document']); this.text = new Group('ADBE Text Properties', [st]); this.text.alias = ['Text']; this.text.sourceText = st; groups.push(this.text); this._st = st; }
    this.root = new Group('root', groups); }
  property(x) { if (x === 'Source Text' || x === 'ADBE Text Document') { if (!this._st) throw new Error('pas un calque texte'); return this._st; } return this.root.property(x); }
  get Effects() { return this.effects; }
  sourceRectAtTime() { return {left: -200, top: -60, width: 400, height: 80}; }
  duplicate() { return this.comp.layers._add(new Layer(this.comp, this.name + ' 2')); }
  remove() { this.comp.layers.items.splice(this.comp.layers.items.indexOf(this), 1); }
  moveToBeginning() {} moveAfter() {} moveBefore() {}
}
class Layers { constructor(c) { this.c = c; this.items = []; } _add(l) { this.items.unshift(l); this.items.forEach((x, i) => x.index = i + 1); return l; }
  get length() { return this.items.length; }
  addText(t) { if (t !== undefined && typeof t !== 'string') throw new Error('addText : texte attendu'); return this._add(new Layer(this.c, 'Texte ' + (++nLayers), t || '')); }
  addSolid(color, name, w, h, par, dur) { if (!Array.isArray(color) || color.length !== 3) throw new Error('addSolid : couleur [r,g,b] attendue'); if (typeof w !== 'number' || typeof h !== 'number' || typeof par !== 'number') throw new Error('addSolid(couleur, nom, largeur, hauteur, pixelAspect[, durée])'); return this._add(new Layer(this.c, name)); }
  addNull(d) { return this._add(new Layer(this.c, 'Null ' + (++nLayers))); }
  addShape() { return this._add(new Layer(this.c, 'Forme ' + (++nLayers))); }
  addCamera(n, c) { if (!Array.isArray(c)) throw new Error('addCamera(nom, [x,y])'); return this._add(new Layer(this.c, n)); }
  precompose(idx, name, move) { if (!Array.isArray(idx)) throw new Error('precompose : tableau d\'index attendu'); return new CompItem(name); } }
class CompItem { constructor(name) { this.name = name; this.width = 1920; this.height = 1080; this.duration = 10; this.frameRate = 30; this.frameDuration = 1 / 30; this.time = 2; this.motionBlur = false; this.layers = new Layers(this); }
  get numLayers() { return this.layers.items.length; } layer(i) { if (typeof i === 'string') return this.layers.items.find(l => l.name === i); const l = this.layers.items[i - 1]; if (!l) throw new Error('layer(' + i + ') hors limites'); return l; }
  get selectedLayers() { return this.layers.items.filter(l => l.selected); } }
const comp = new CompItem('Clip');
const t1 = comp.layers.addText('Salut'); t1.selected = true; t1.name = 'Titre'; comp.layers.addSolid([0.1, 0.1, 0.1], 'Fond', 1920, 1080, 1);
let undo = 0; const out = {};
const app = {project: {activeItem: comp, numItems: 1, item: i => comp}, beginUndoGroup: () => undo++, endUndoGroup: () => undo--};
function File(p) { this.p = p; this.open = () => true; this.write = s => { out.result = s; }; this.close = () => {}; }
const ctx = {app, CompItem, KeyframeEase, PropertyValueType: PVT, KeyframeInterpolationType: {LINEAR: 6612, BEZIER: 6613, HOLD: 6614},
  ParagraphJustification: {LEFT_JUSTIFY: 7413, CENTER_JUSTIFY: 7415, RIGHT_JUSTIFY: 7414}, TextDocument, File, Math, Array, Error, String, Number, $: {writeln: () => {}}, alert: () => {}};
vm.createContext(ctx);
try { vm.runInContext(fs.readFileSync(process.argv[2], 'utf8').replace(/^﻿/, ''), ctx); } catch (e) { out.result = 'PLANTAGE JS|' + e.message; }
const summary = comp.layers.items.map(l => l.name + ' [' + l.transform.list.filter(p => p.keys.length || p.expression).map(p => p.name + (p.keys.length ? ':' + p.keys.length + 'clés' : '') + (p.expression ? ' expr=' + p.expression : '')).join(', ') + (l.effects.list.length ? ' effets=' + l.effects.list.map(e => e.name).join('+') : '') + (l._st ? ' texte="' + (l._st.value.text || l._st.value) + '"' : '') + (l.motionBlur ? ' flou-mvt' : '') + ']').join(' | ');
console.log((out.result || '(aucun rapport)') + '\n  calques : ' + summary + (undo ? '\n  !! groupe d\'annulation non fermé' : ''));
