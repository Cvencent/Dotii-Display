/* Canvas renderer for the Dotii expression vocabulary. */
(() => {
  const TAU = Math.PI * 2;

  function clamp(value, min, max) { return Math.max(min, Math.min(max, value)); }
  function lerp(a, b, t) { return a + (b - a) * t; }
  function easeInOut(t) { return t < .5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2; }
  function hexRgb(hex) {
    const value = parseInt(hex.replace('#', ''), 16);
    return { r: (value >> 16) & 255, g: (value >> 8) & 255, b: value & 255 };
  }
  function mix(a, b, amount) {
    const ca = hexRgb(a), cb = hexRgb(b);
    const r = Math.round(lerp(ca.r, cb.r, amount));
    const g = Math.round(lerp(ca.g, cb.g, amount));
    const bl = Math.round(lerp(ca.b, cb.b, amount));
    return `rgb(${r} ${g} ${bl})`;
  }
  function rgba(hex, alpha) {
    const c = hexRgb(hex);
    return `rgba(${c.r}, ${c.g}, ${c.b}, ${alpha})`;
  }

  const DEFAULT_STATE = {
    eyeShape: 'open', leftEyeShape: null, rightEyeShape: null,
    eyeOpen: 1, eyeW: 1, eyeH: 1, eyeX: 0, eyeY: 0,
    pupilX: 0, pupilY: 0,
    brow: 'neutral', browY: 0, browTilt: 0,
    mouth: 'flat', mouthOpen: 0, mouthW: 1, mouthCurve: 0,
    blush: 0, headX: 0, headY: 0, headRot: 0, headScale: 1, dim: 1,
    effects: [],
  };

  function interpolateTimeline(expression, t) {
    const timeline = expression.timeline;
    if (!timeline || timeline.length === 0) return {};
    if (timeline.length === 1) return timeline[0];
    let index = 0;
    while (index < timeline.length - 1 && t > timeline[index + 1].at) index += 1;
    const from = timeline[index];
    const to = timeline[Math.min(index + 1, timeline.length - 1)];
    const span = Math.max(0.0001, to.at - from.at);
    const amount = easeInOut(clamp((t - from.at) / span, 0, 1));
    const result = {};
    const keys = new Set([...Object.keys(from), ...Object.keys(to)]);
    keys.delete('at');
    keys.forEach((key) => {
      const a = from[key] === undefined ? (DEFAULT_STATE[key] ?? to[key] ?? 0) : from[key];
      const b = to[key] === undefined ? (DEFAULT_STATE[key] ?? from[key] ?? 0) : to[key];
      if (typeof a === 'number' && typeof b === 'number') result[key] = lerp(a, b, amount);
    });
    return result;
  }

  function expressionState(expression, timeMs) {
    const duration = Math.max(400, expression.duration || 3000);
    const t = (timeMs % duration) / duration;
    const state = Object.assign({}, DEFAULT_STATE, expression.base || {});
    Object.assign(state, interpolateTimeline(expression, t));
    (expression.motion || []).forEach((motion) => {
      const period = Math.max(120, motion.period || duration);
      const wave = Math.sin(TAU * (timeMs / period) + (motion.phase || 0));
      state[motion.channel] = (state[motion.channel] ?? 0) + wave * motion.amp;
    });
    state.effects = (expression.base && expression.base.effects) || [];
    return { state, t };
  }

  function roundedRect(ctx, x, y, w, h, radius) {
    const r = Math.min(radius, w / 2, h / 2);
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + w, y, x + w, y + h, r);
    ctx.arcTo(x + w, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r);
    ctx.arcTo(x, y, x + w, y, r);
    ctx.closePath();
  }

  function drawStar(ctx, x, y, outer, inner, points = 5) {
    ctx.beginPath();
    for (let i = 0; i < points * 2; i += 1) {
      const radius = i % 2 === 0 ? outer : inner;
      const angle = -Math.PI / 2 + (i * Math.PI) / points;
      const px = x + Math.cos(angle) * radius;
      const py = y + Math.sin(angle) * radius;
      if (i === 0) ctx.moveTo(px, py); else ctx.lineTo(px, py);
    }
    ctx.closePath();
  }

  function drawHeartPath(ctx, x, y, size) {
    ctx.beginPath();
    ctx.moveTo(x, y + size * .32);
    ctx.bezierCurveTo(x - size * .72, y - size * .22, x - size * .38, y - size * .8, x, y - size * .34);
    ctx.bezierCurveTo(x + size * .38, y - size * .8, x + size * .72, y - size * .22, x, y + size * .32);
    ctx.closePath();
  }

  function drawEye(ctx, cx, cy, w, h, shape, state, side, color, body) {
    const open = clamp(state.eyeOpen, .02, 1.8);
    const width = w * clamp(state.eyeW, .45, 1.5);
    const height = h * clamp(state.eyeH, .25, 1.6) * open;
    const eyeX = state.eyeX * w * .12 + (side === 'left' ? -state.eyeX : state.eyeX);
    const eyeY = state.eyeY * h * .15;

    if (shape === 'x') {
      ctx.strokeStyle = color;
      ctx.lineWidth = Math.max(2, w * .22);
      ctx.lineCap = 'round';
      const arm = Math.min(width, Math.max(4, height)) * .42;
      ctx.beginPath();
      ctx.moveTo(cx - arm, cy - arm); ctx.lineTo(cx + arm, cy + arm);
      ctx.moveTo(cx + arm, cy - arm); ctx.lineTo(cx - arm, cy + arm);
      ctx.stroke();
      return;
    }

    if (shape === 'heart') {
      drawHeartPath(ctx, cx, cy, Math.max(width, height) * .52);
      ctx.fillStyle = color;
      ctx.fill();
      return;
    }

    if (shape === 'star') {
      drawStar(ctx, cx, cy, Math.max(width, height) * .62, Math.max(width, height) * .27);
      ctx.fillStyle = color;
      ctx.fill();
      return;
    }

    if (shape === 'happy' || shape === 'crescent' || shape === 'closed') {
      const isHappy = shape === 'happy';
      const lift = shape === 'crescent' ? .24 : .4;
      ctx.strokeStyle = color;
      ctx.lineWidth = Math.max(2.2, w * (shape === 'closed' ? .18 : .16));
      ctx.lineCap = 'round';
      ctx.beginPath();
      if (isHappy) {
        ctx.moveTo(cx - width * .52, cy - height * .12);
        ctx.quadraticCurveTo(cx, cy + height * lift, cx + width * .52, cy - height * .12);
      } else {
        ctx.moveTo(cx - width * .52, cy + height * .16);
        ctx.quadraticCurveTo(cx, cy - height * lift, cx + width * .52, cy + height * .16);
      }
      ctx.stroke();
      return;
    }

    if (shape === 'scan' || shape === 'loading' || shape === 'target') {
      const rw = width * .58, rh = Math.max(2.2, height * .24);
      roundedRect(ctx, cx + eyeX - rw / 2, cy + eyeY - rh / 2, rw, rh, rh * .45);
      ctx.fillStyle = color;
      ctx.fill();
      if (shape === 'loading') {
        ctx.strokeStyle = rgba('#ffffff', .42);
        ctx.lineWidth = Math.max(1, w * .06);
        ctx.beginPath();
        ctx.arc(cx + eyeX, cy + eyeY, rw * .9, -Math.PI / 2, Math.PI * .1);
        ctx.stroke();
      }
      if (shape === 'target') {
        ctx.strokeStyle = rgba('#ffffff', .72);
        ctx.lineWidth = Math.max(1, w * .05);
        ctx.beginPath();
        ctx.moveTo(cx - rw, cy + eyeY); ctx.lineTo(cx + rw, cy + eyeY);
        ctx.moveTo(cx + eyeX, cy - rh * 3); ctx.lineTo(cx + eyeX, cy + rh * 3);
        ctx.stroke();
      }
      return;
    }

    ctx.save();
    ctx.translate(cx + eyeX, cy + eyeY);
    let rotation = 0;
    if (shape === 'sad') rotation = side === 'left' ? .16 : -.16;
    if (shape === 'sleepy') rotation = side === 'left' ? -.05 : .05;
    ctx.rotate(rotation);
    ctx.beginPath();
    ctx.ellipse(0, 0, Math.max(2, width * .5), Math.max(2, height * .5), 0, 0, TAU);
    ctx.fillStyle = color;
    ctx.fill();

    if (shape === 'narrow') {
      ctx.fillStyle = body;
      ctx.fillRect(-width * .62, -height * .68, width * 1.24, height * .44);
      ctx.fillRect(-width * .62, height * .25, width * 1.24, height * .5);
    } else if (shape === 'sleepy') {
      ctx.fillStyle = body;
      ctx.fillRect(-width * .62, -height * .72, width * 1.24, height * .42);
    } else if (shape === 'wide') {
      ctx.strokeStyle = rgba('#ffffff', .52);
      ctx.lineWidth = Math.max(1, w * .04);
      ctx.stroke();
    }

    if (shape !== 'wide' || height > 5) {
      const pupilScale = shape === 'wide' ? .34 : .38;
      ctx.beginPath();
      ctx.ellipse(
        state.pupilX * width * .22,
        state.pupilY * height * .24,
        Math.max(1.3, width * pupilScale * .5),
        Math.max(1.3, height * pupilScale * .5),
        0, 0, TAU
      );
      ctx.fillStyle = mix('#25333b', '#ffffff', shape === 'wide' ? .12 : .05);
      ctx.fill();
    }
    ctx.restore();
  }

  function drawBrow(ctx, cx, cy, width, state, side, color) {
    const type = state.brow || 'neutral';
    const browY = state.browY * .7;
    const tilt = state.browTilt * .8;
    const inner = type === 'focused' || type === 'angry' ? 1 : type === 'worried' || type === 'sad' ? -1 : 0;
    const raised = type === 'raised' || type === 'asym_raised';
    const asymmetric = type === 'asym_raised' && side === 'left';
    ctx.strokeStyle = color;
    ctx.lineWidth = Math.max(1.6, width * .14);
    ctx.lineCap = 'round';
    ctx.beginPath();
    const half = width * .52;
    const y = cy + browY - (raised ? width * .22 : 0) - (asymmetric ? width * .18 : 0);
    const innerY = y + inner * width * .22 + tilt;
    const outerY = y - inner * width * .08 - tilt;
    if (side === 'left') {
      ctx.moveTo(cx - half, outerY);
      ctx.quadraticCurveTo(cx - half * .15, y + (type === 'raised' ? -width * .1 : 0), cx + half, innerY);
    } else {
      ctx.moveTo(cx + half, outerY);
      ctx.quadraticCurveTo(cx + half * .15, y + (type === 'raised' ? -width * .1 : 0), cx - half, innerY);
    }
    ctx.stroke();
  }

  function drawMouth(ctx, cx, cy, width, state, color) {
    const type = state.mouth || 'flat';
    const w = width * clamp(state.mouthW, .55, 1.35);
    const open = clamp(state.mouthOpen, 0, 1);
    const curve = state.mouthCurve || 0;
    ctx.strokeStyle = color;
    ctx.fillStyle = color;
    ctx.lineWidth = Math.max(2, width * .075);
    ctx.lineCap = 'round';

    if (type === 'o') {
      ctx.beginPath();
      ctx.ellipse(cx, cy, w * .18 + open * w * .12, w * .2 + open * w * .18, 0, 0, TAU);
      ctx.fill();
      return;
    }
    if (type === 'open_smile' || type === 'open') {
      const h = w * (.16 + open * .52);
      ctx.beginPath();
      ctx.ellipse(cx, cy + h * .28, w * .58, h, 0, 0, TAU);
      ctx.fill();
      return;
    }
    if (type === 'wavy') {
      ctx.beginPath();
      ctx.moveTo(cx - w * .52, cy);
      ctx.quadraticCurveTo(cx - w * .3, cy - w * .23, cx - w * .08, cy);
      ctx.quadraticCurveTo(cx + w * .16, cy + w * .23, cx + w * .36, cy);
      ctx.quadraticCurveTo(cx + w * .5, cy - w * .08, cx + w * .54, cy - w * .02);
      ctx.stroke();
      return;
    }
    if (type === 'smirk') {
      ctx.beginPath();
      ctx.moveTo(cx - w * .5, cy + w * .02);
      ctx.quadraticCurveTo(cx + w * .05, cy + w * .06, cx + w * .52, cy - w * .3);
      ctx.stroke();
      return;
    }
    if (type === 'frown' || type === 'frown_small') {
      const small = type === 'frown_small';
      const span = small ? w * .36 : w * .52;
      ctx.beginPath();
      ctx.moveTo(cx - span, cy + w * .18);
      ctx.quadraticCurveTo(cx, cy - w * (.48 + Math.abs(curve) * .15), cx + span, cy + w * .18);
      ctx.stroke();
      return;
    }
    if (type === 'smile' || type === 'small_smile') {
      const small = type === 'small_smile';
      const span = small ? w * .36 : w * .52;
      const amount = small ? .22 : .46 * Math.max(.5, curve);
      ctx.beginPath();
      ctx.moveTo(cx - span, cy - w * .08);
      ctx.quadraticCurveTo(cx, cy + w * amount, cx + span, cy - w * .08);
      ctx.stroke();
      return;
    }
    const span = type === 'small' ? w * .34 : w * .5;
    ctx.beginPath();
    ctx.moveTo(cx - span, cy);
    ctx.quadraticCurveTo(cx, cy + curve * w * .2, cx + span, cy);
    ctx.stroke();
  }

  function drawBlush(ctx, cx, cy, width, state, accent) {
    if (state.blush < .04) return;
    const alpha = clamp(state.blush * .5, 0, .48);
    [-1, 1].forEach((side) => {
      const x = cx + side * width * .39;
      const y = cy - width * .09;
      const gradient = ctx.createRadialGradient(x, y, 0, x, y, width * .13);
      gradient.addColorStop(0, rgba(accent, alpha));
      gradient.addColorStop(1, rgba(accent, 0));
      ctx.fillStyle = gradient;
      ctx.beginPath();
      ctx.ellipse(x, y, width * .13, width * .075, 0, 0, TAU);
      ctx.fill();
    });
  }

  function drawEffect(ctx, type, size, state, t, accent) {
    const pulse = .5 + .5 * Math.sin(TAU * t);
    ctx.save();
    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';
    if (type === 'sparkle') {
      for (let i = 0; i < 4; i += 1) {
        const angle = t * TAU + i * Math.PI / 2;
        const x = size * (.5 + Math.cos(angle) * .36);
        const y = size * (.5 + Math.sin(angle) * .34);
        const r = size * (.014 + .012 * pulse);
        ctx.strokeStyle = rgba(accent, .55 + .35 * pulse);
        ctx.lineWidth = Math.max(1, size * .006);
        ctx.beginPath();
        ctx.moveTo(x - r, y); ctx.lineTo(x + r, y);
        ctx.moveTo(x, y - r); ctx.lineTo(x, y + r);
        ctx.stroke();
      }
    } else if (type === 'zzz') {
      for (let i = 0; i < 3; i += 1) {
        const phase = (t + i / 3) % 1;
        const alpha = Math.sin(Math.PI * phase) * .7;
        const x = size * (.68 + i * .055 + phase * .04);
        const y = size * (.31 - phase * .2 + i * .02);
        ctx.fillStyle = rgba('#cbd6e5', alpha);
        ctx.font = `700 ${Math.max(8, size * .07 * (1 - i * .08))}px ui-monospace, monospace`;
        ctx.textAlign = 'center';
        ctx.fillText('Z', x, y);
      }
    } else if (type === 'rings') {
      const level = 1 - (t % .5) * 2;
      [-1, 1].forEach((side) => {
        ctx.strokeStyle = rgba(accent, .18 + level * .38);
        ctx.lineWidth = Math.max(1, size * .007);
        for (let i = 0; i < 3; i += 1) {
          const radius = size * (.075 + i * .055) * (1 - level * .18);
          ctx.beginPath();
          ctx.arc(size * (.5 + side * .47), size * .49, radius, side < 0 ? Math.PI * .55 : -Math.PI * .45, side < 0 ? Math.PI * 1.45 : Math.PI * .45);
          ctx.stroke();
        }
      });
    } else if (type === 'orbit') {
      for (let i = 0; i < 3; i += 1) {
        const angle = t * TAU + i * TAU / 3;
        const x = size * (.5 + Math.cos(angle) * .25);
        const y = size * (.25 + Math.sin(angle) * .08);
        ctx.fillStyle = rgba(accent, .65);
        ctx.beginPath();
        ctx.arc(x, y, size * .016, 0, TAU);
        ctx.fill();
      }
      ctx.strokeStyle = rgba(accent, .24);
      ctx.lineWidth = Math.max(1, size * .004);
      ctx.beginPath();
      ctx.ellipse(size * .5, size * .25, size * .25, size * .08, 0, 0, TAU);
      ctx.stroke();
    } else if (type === 'bars') {
      for (let i = 0; i < 5; i += 1) {
        const height = size * (.035 + .07 * (0.5 + 0.5 * Math.sin(t * TAU * 2 + i * .8)));
        const x = size * (.79 + i * .033);
        const y = size * .66 - height;
        ctx.fillStyle = rgba(accent, .5 + .3 * (height / (size * .105)));
        roundedRect(ctx, x, y, size * .017, height, size * .009);
        ctx.fill();
      }
    } else if (type === 'check' || type === 'cross' || type === 'warning' || type === 'exclamation' || type === 'question') {
      const x = size * .79, y = size * .34;
      const radius = size * .055;
      ctx.fillStyle = type === 'cross' ? rgba('#ff735c', .92) : rgba(accent, .9);
      ctx.beginPath();
      ctx.arc(x, y, radius, 0, TAU);
      ctx.fill();
      ctx.strokeStyle = '#081018';
      ctx.lineWidth = Math.max(1.4, size * .009);
      if (type === 'check') {
        ctx.beginPath(); ctx.moveTo(x - radius * .45, y); ctx.lineTo(x - radius * .08, y + radius * .36); ctx.lineTo(x + radius * .5, y - radius * .38); ctx.stroke();
      } else {
        ctx.beginPath(); ctx.moveTo(x, y - radius * .45); ctx.lineTo(x, y + radius * .1); ctx.moveTo(x, y + radius * .42); ctx.lineTo(x, y + radius * .44); ctx.stroke();
      }
    } else if (type === 'hearts') {
      for (let i = 0; i < 3; i += 1) {
        const phase = (t + i / 3) % 1;
        const alpha = Math.sin(Math.PI * phase) * .8;
        drawHeartPath(ctx, size * (.72 + i * .05 + Math.sin(t * TAU + i) * .015), size * (.56 - phase * .34), size * (.055 + i * .008));
        ctx.fillStyle = rgba('#ff82ac', alpha);
        ctx.fill();
      }
    } else if (type === 'tear') {
      const drop = (t * 1.5) % 1;
      ctx.fillStyle = rgba('#8ed5ff', .75);
      ctx.beginPath();
      ctx.ellipse(size * .28, size * (.54 + drop * .16), size * .018, size * .032, 0, 0, TAU);
      ctx.fill();
    } else if (type === 'ellipsis') {
      for (let i = 0; i < 3; i += 1) {
        const alpha = .2 + .65 * (0.5 + 0.5 * Math.sin(t * TAU - i * .8));
        ctx.fillStyle = rgba(accent, alpha);
        ctx.beginPath();
        ctx.arc(size * (.7 + i * .065), size * .32, size * .014, 0, TAU);
        ctx.fill();
      }
    } else if (type === 'slash') {
      ctx.strokeStyle = rgba('#ff735c', .72);
      ctx.lineWidth = Math.max(2, size * .011);
      ctx.beginPath();
      ctx.moveTo(size * .68, size * .26);
      ctx.lineTo(size * .86, size * .58);
      ctx.stroke();
    } else if (type === 'target') {
      ctx.strokeStyle = rgba(accent, .55);
      ctx.lineWidth = Math.max(1, size * .006);
      ctx.beginPath(); ctx.arc(size * .5, size * .47, size * .34, 0, TAU); ctx.stroke();
      ctx.beginPath(); ctx.arc(size * .5, size * .47, size * .26, 0, TAU); ctx.stroke();
    } else if (type === 'progress' || type === 'loading') {
      ctx.strokeStyle = rgba(accent, .22);
      ctx.lineWidth = Math.max(2, size * .012);
      ctx.beginPath(); ctx.arc(size * .5, size * .2, size * .075, 0, TAU); ctx.stroke();
      ctx.strokeStyle = rgba(accent, .92);
      ctx.beginPath(); ctx.arc(size * .5, size * .2, size * .075, t * TAU, t * TAU + Math.PI * .75); ctx.stroke();
    } else if (type === 'note') {
      for (let i = 0; i < 2; i += 1) {
        const phase = (t + i * .5) % 1;
        ctx.fillStyle = rgba(accent, Math.sin(Math.PI * phase) * .8);
        ctx.beginPath(); ctx.arc(size * (.72 + i * .04), size * (.58 - phase * .3), size * .022, 0, TAU); ctx.fill();
        ctx.fillRect(size * (.74 + i * .04), size * (.52 - phase * .3), size * .009, size * .08);
      }
    } else if (type === 'notification') {
      ctx.strokeStyle = rgba(accent, .72);
      ctx.lineWidth = Math.max(1.4, size * .008);
      ctx.beginPath();
      ctx.moveTo(size * .86, size * .26);
      ctx.lineTo(size * .7, size * .26);
      ctx.stroke();
    } else if (type === 'anger') {
      const strength = .45 + .45 * pulse;
      [-1, 1].forEach((side) => {
        ctx.strokeStyle = rgba('#ff735c', strength);
        ctx.lineWidth = Math.max(1.5, size * .008);
        for (let i = 0; i < 2; i += 1) {
          const x = size * (.5 + side * (.36 + i * .045));
          ctx.beginPath();
          ctx.moveTo(x, size * .2 + i * size * .03);
          ctx.lineTo(x + side * size * .035, size * .25 + i * size * .03);
          ctx.stroke();
        }
      });
    } else if (type === 'stars') {
      for (let i = 0; i < 5; i += 1) {
        const angle = t * TAU + i * TAU / 5;
        const radius = size * (.28 + .08 * pulse);
        drawStar(ctx, size * (.5 + Math.cos(angle) * radius), size * (.5 + Math.sin(angle) * radius), size * .018, size * .008);
        ctx.fillStyle = rgba(accent, .42 + .38 * pulse);
        ctx.fill();
      }
    } else if (type === 'plus') {
      ctx.strokeStyle = rgba(accent, .7);
      ctx.lineWidth = Math.max(1, size * .008);
      const x = size * .82, y = size * .34, r = size * .028;
      ctx.beginPath(); ctx.moveTo(x - r, y); ctx.lineTo(x + r, y); ctx.moveTo(x, y - r); ctx.lineTo(x, y + r); ctx.stroke();
    } else if (type === 'moon') {
      ctx.fillStyle = rgba('#c7b7ff', .72);
      ctx.beginPath(); ctx.arc(size * .78, size * .27, size * .04, 0, TAU); ctx.fill();
      ctx.fillStyle = rgba('#0b0e13', .92);
      ctx.beginPath(); ctx.arc(size * .795, size * .255, size * .039, 0, TAU); ctx.fill();
    } else if (type === 'battery_low' || type === 'charging') {
      const x = size * .75, y = size * .25, w = size * .13, h = size * .065;
      ctx.strokeStyle = rgba(type === 'battery_low' ? '#ff735c' : accent, .8);
      ctx.lineWidth = Math.max(1, size * .007);
      roundedRect(ctx, x, y, w, h, size * .012); ctx.stroke();
      ctx.fillStyle = rgba(type === 'battery_low' ? '#ff735c' : accent, .8);
      ctx.fillRect(x + w + size * .01, y + h * .25, size * .018, h * .5);
      if (type === 'charging') {
        ctx.beginPath(); ctx.moveTo(x + w * .55, y + h * .08); ctx.lineTo(x + w * .32, y + h * .52); ctx.lineTo(x + w * .56, y + h * .48); ctx.lineTo(x + w * .42, y + h * .92); ctx.strokeStyle = '#effaf4'; ctx.stroke();
      }
    } else if (type === 'mute') {
      ctx.strokeStyle = rgba(accent, .72);
      ctx.lineWidth = Math.max(1, size * .008);
      ctx.beginPath(); ctx.moveTo(size * .76, size * .3); ctx.lineTo(size * .84, size * .38); ctx.moveTo(size * .84, size * .3); ctx.lineTo(size * .76, size * .38); ctx.stroke();
    } else if (type === 'lock') {
      const x = size * .78, y = size * .31;
      ctx.strokeStyle = rgba(accent, .76);
      ctx.lineWidth = Math.max(1.2, size * .008);
      roundedRect(ctx, x - size * .035, y, size * .07, size * .055, size * .012); ctx.stroke();
      ctx.beginPath(); ctx.arc(x, y, size * .025, Math.PI, 0); ctx.stroke();
    }
    ctx.restore();
  }

  class FaceRenderer {
    constructor(canvas) {
      this.canvas = canvas;
      this.ctx = canvas.getContext('2d');
      this.expression = null;
      this.size = 0;
      this.dpr = 1;
      this.card = canvas.dataset.mode === 'card';
    }

    setExpression(expression) { this.expression = expression; }

    resize() {
      const rect = this.canvas.getBoundingClientRect();
      const cssSize = Math.max(1, Math.min(rect.width, rect.height) || rect.width || 1);
      const dpr = Math.min(2, window.devicePixelRatio || 1);
      const pixelSize = Math.round(cssSize * dpr);
      if (this.canvas.width !== pixelSize || this.canvas.height !== pixelSize) {
        this.canvas.width = pixelSize;
        this.canvas.height = pixelSize;
      }
      this.size = cssSize;
      this.dpr = dpr;
    }

    render(timeMs) {
      if (!this.expression) return;
      this.resize();
      const ctx = this.ctx;
      const size = this.size;
      const accent = this.expression.accent || '#65c6ff';
      const { state } = expressionState(this.expression, timeMs);
      const alpha = clamp(state.dim ?? 1, .35, 1);
      ctx.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
      ctx.clearRect(0, 0, size, size);

      const bodyColor = mix('#36446e', accent, this.card ? .24 : .28);
      const eyeColor = mix('#f1f8f5', accent, this.card ? .08 : .04);
      const strokeColor = mix(eyeColor, '#ffffff', .08);

      ctx.save();
      ctx.translate(size / 2, size / 2);
      ctx.rotate((state.headRot || 0) * Math.PI / 180);
      ctx.scale(state.headScale || 1, state.headScale || 1);
      ctx.translate((state.headX || 0) * size * .004, (state.headY || 0) * size * .004);
      ctx.globalAlpha = alpha;

      const gradient = ctx.createRadialGradient(-size * .16, -size * .2, size * .03, 0, 0, size * .57);
      gradient.addColorStop(0, mix(bodyColor, '#ffffff', .13));
      gradient.addColorStop(1, mix(bodyColor, '#000000', .13));
      ctx.fillStyle = gradient;
      ctx.beginPath();
      ctx.arc(0, 0, size * .472, 0, TAU);
      ctx.fill();

      ctx.strokeStyle = rgba(accent, .22);
      ctx.lineWidth = Math.max(1, size * .009);
      ctx.beginPath();
      ctx.arc(0, 0, size * .468, 0, TAU);
      ctx.stroke();

      const eyeY = size * .43;
      const eyeSeparation = size * .17;
      const eyeW = size * .17;
      const eyeH = size * .185;
      const leftShape = state.leftEyeShape || state.eyeShape;
      const rightShape = state.rightEyeShape || state.eyeShape;

      drawBrow(ctx, -eyeSeparation, eyeY - size * .125, eyeW * .86, state, 'left', strokeColor);
      drawBrow(ctx, eyeSeparation, eyeY - size * .125, eyeW * .86, state, 'right', strokeColor);
      drawBlush(ctx, 0, size * .65, size, state, accent);
      drawEye(ctx, -eyeSeparation, eyeY, eyeW, eyeH, leftShape, state, 'left', eyeColor, bodyColor);
      drawEye(ctx, eyeSeparation, eyeY, eyeW, eyeH, rightShape, state, 'right', eyeColor, bodyColor);
      drawMouth(ctx, 0, size * .7, size * .22, state, strokeColor);
      ctx.restore();

      state.effects.forEach((effect) => drawEffect(ctx, effect, size, state, (timeMs % 2600) / 2600, accent));
      ctx.globalAlpha = 1;
    }
  }

  window.DotiiFace = { FaceRenderer, expressionState };
})();
