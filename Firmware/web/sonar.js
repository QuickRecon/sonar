(function () {
    "use strict";

    /* ---- Color Palette LUTs (256-entry RGBA arrays) ---- */

    function buildPalette(fn) {
        var lut = new Uint8Array(256 * 4);
        for (var i = 0; i < 256; i++) {
            var rgba = fn(i);
            lut[i * 4]     = rgba[0];
            lut[i * 4 + 1] = rgba[1];
            lut[i * 4 + 2] = rgba[2];
            lut[i * 4 + 3] = rgba[3];
        }
        return lut;
    }

    var palettes = {
        grayscale: buildPalette(function (v) {
            return [v, v, v, 255];
        }),
        ocean: buildPalette(function (v) {
            var t = v / 255;
            if (t < 0.5) {
                var s = t * 2;
                return [0, Math.round(s * 100), Math.round(50 + s * 205), 255];
            }
            var s = (t - 0.5) * 2;
            return [Math.round(s * 255), Math.round(100 + s * 155), Math.round(255 - s * 55), 255];
        }),
        turbo: buildPalette(function (v) {
            var stops = [
                [0.00, 48, 18, 59],
                [0.13, 69, 91, 205],
                [0.25, 40, 165, 225],
                [0.38, 34, 221, 165],
                [0.50, 121, 252, 76],
                [0.63, 213, 237, 36],
                [0.75, 249, 176, 14],
                [0.88, 239, 90, 17],
                [1.00, 122, 4, 3]
            ];
            var t = v / 255;
            var i = 0;
            while (i < stops.length - 2 && stops[i + 1][0] < t) i++;
            var s0 = stops[i], s1 = stops[i + 1];
            var f = (t - s0[0]) / (s1[0] - s0[0]);
            return [
                Math.round(s0[1] + f * (s1[1] - s0[1])),
                Math.round(s0[2] + f * (s1[2] - s0[2])),
                Math.round(s0[3] + f * (s1[3] - s0[3])),
                255
            ];
        }),
        hot: buildPalette(function (v) {
            var t = v / 255;
            var r, g, b;
            if (t < 0.33) {
                r = Math.round(t / 0.33 * 255);
                g = 0;
                b = 0;
            } else if (t < 0.66) {
                r = 255;
                g = Math.round((t - 0.33) / 0.33 * 255);
                b = 0;
            } else {
                r = 255;
                g = 255;
                b = Math.round((t - 0.66) / 0.34 * 255);
            }
            return [r, g, b, 255];
        })
    };

    var currentPalette = palettes.turbo;

    /* Pre-built CSS color strings for fast arc fills */
    var colorLUT = new Array(256);
    function rebuildColorLUT() {
        for (var i = 0; i < 256; i++) {
            colorLUT[i] = "rgb(" + currentPalette[i * 4] + "," +
                          currentPalette[i * 4 + 1] + "," +
                          currentPalette[i * 4 + 2] + ")";
        }
    }
    rebuildColorLUT();

    /* ---- Canvas setup ---- */

    var canvas = document.getElementById("sonar-canvas");
    var ctx = canvas.getContext("2d");
    var MARGIN = 20;

    /* Viewport — updated dynamically by resizeCanvas() / computeViewport() */
    var W = 2, H = 2;
    var cx = 1, cy = 1, radius = 1;
    var fullCircle = true;

    /* Offscreen canvas for persistent sonar image */
    var offCanvas = document.createElement("canvas");
    var offCtx = offCanvas.getContext("2d");
    function clearOffscreen() {
        offCtx.fillStyle = "#000000";
        offCtx.fillRect(0, 0, W, H);
    }

    /* ---- Sound mode offscreen canvas (spectrogram) ---- */
    var soundCanvas = document.createElement("canvas");
    var soundCtx = soundCanvas.getContext("2d");
    function clearSoundCanvas() {
        soundCtx.fillStyle = "#000000";
        soundCtx.fillRect(0, 0, W, H);
    }

    /* ---- State ---- */

    var currentMode = "scan";
    var savedSector = { start: 0, end: 399 };

    var config = {
        gain: 1,
        start_angle: 0,
        end_angle: 399,
        num_samples: 200,
        transmit_frequency: 750,
        transmit_duration: 0,
        sample_period: 0,
        range_mm: 5000,
        speed_of_sound: 1500,
        saltwater: true
    };

    var sonarStore = new Array(400);  // indexed by gradian angle
    // Each entry: { data: Uint8Array, numSamples: number, range_mm: number } or null

    var needsRedraw = true;
    var pendingClear = false;
    var rxFrameCount = 0;

    /* Display transform state (client-side only) */
    var offsetGrad = 0;      // 0, 50, 100, ..., 350 gradians
    var mirrorDisplay = false;
    var currentDepth = null;  // latest depth from status messages

    /* Compass state */
    var compassEnabled = false;
    var compassHeading = null;     // degrees, 0=N, 90=E, null=unavailable
    var lastRedrawHeading = null;  // heading at last full redraw
    var COMPASS_MARGIN = 45;

    /* ---- Display transform ---- */

    /** Map a sonar gradian to a display gradian.
     *  Subtracts offset (un-rotates mounting misalignment) then applies mirror. */
    function displayGrad(grad) {
        var g = (grad - offsetGrad + 400) % 400;
        return mirrorDisplay ? (400 - g) % 400 : g;
    }

    /* ---- Viewport / dynamic resize ---- */

    /** Check if display angle dAngle falls within the display sector
     *  that sweeps clockwise from dStart to dEnd. */
    function dAngleInRange(dAngle, dStart, dEnd) {
        if (dStart <= dEnd) return dAngle >= dStart && dAngle <= dEnd;
        return dAngle >= dStart || dAngle <= dEnd;
    }

    /** Compute bounding box of sector arc + origin in normalised coords. */
    function sectorBoundingBox() {
        var dStart = displayGrad(config.start_angle);
        var dEnd = displayGrad(config.end_angle);
        if (mirrorDisplay) { var tmp = dStart; dStart = dEnd; dEnd = tmp; }

        /* Collect extreme-candidate points (normalised, radius=1) */
        var pts = [[0, 0]]; /* origin */

        /* Arc endpoints in display space */
        var sA = gradToRad(dStart);
        pts.push([Math.cos(sA), -Math.sin(sA)]);
        var eA = gradToRad(dEnd);
        pts.push([Math.cos(eA), -Math.sin(eA)]);

        /* Cardinal directions that fall inside the display sector */
        for (var c = 0; c < 400; c += 100) {
            if (dAngleInRange(c, dStart, dEnd)) {
                var a = gradToRad(c);
                pts.push([Math.cos(a), -Math.sin(a)]);
            }
        }

        var minX = Infinity, maxX = -Infinity;
        var minY = Infinity, maxY = -Infinity;
        for (var i = 0; i < pts.length; i++) {
            if (pts[i][0] < minX) minX = pts[i][0];
            if (pts[i][0] > maxX) maxX = pts[i][0];
            if (pts[i][1] < minY) minY = pts[i][1];
            if (pts[i][1] > maxY) maxY = pts[i][1];
        }
        return { minX: minX, maxX: maxX, minY: minY, maxY: maxY };
    }

    /** Recompute cx, cy, radius to optimally fill the current canvas. */
    function computeViewport() {
        var start = config.start_angle;
        var end = config.end_angle;
        fullCircle = (start === 0 && end === 399);
        var margin = compassEnabled ? COMPASS_MARGIN : MARGIN;

        if (fullCircle || start === end) {
            cx = W / 2;
            cy = H / 2;
            radius = Math.min(W, H) / 2 - margin;
        } else {
            var bb = sectorBoundingBox();
            var bbW = bb.maxX - bb.minX;
            var bbH = bb.maxY - bb.minY;

            /* Largest radius that keeps the sector inside the canvas */
            radius = Math.min((W - 2 * margin) / bbW,
                              (H - 2 * margin) / bbH);

            /* Centre the sector bounding box within the canvas */
            var sectorPxW = bbW * radius;
            var sectorPxH = bbH * radius;
            cx = (W - sectorPxW) / 2 + (-bb.minX) * radius;
            cy = (H - sectorPxH) / 2 + (-bb.minY) * radius;
        }
    }

    /** Resize canvas to fill its container, recompute viewport, redraw. */
    function resizeCanvas() {
        var container = document.querySelector(".main-panel");
        var availW = container.clientWidth;
        var availH = container.clientHeight;
        if (availW < 1 || availH < 1) return;

        W = availW;
        H = availH;
        canvas.width  = W;
        canvas.height = H;
        offCanvas.width  = W;
        offCanvas.height = H;
        soundCanvas.width  = W;
        soundCanvas.height = H;

        computeViewport();
        redrawAll();
        clearSoundCanvas();
        needsRedraw = true;
    }

    function updateViewport() {
        computeViewport();
        redrawAll();
    }

    /* ---- Gradian to radians conversion ---- */
    /* Ping360: 0 gradians = forward, increases clockwise */
    /* Canvas: 0 rad = right, increases counter-clockwise */
    /* Mapping: canvas_angle = PI/2 - (gradian * 2*PI / 400) */

    function gradToRad(grad) {
        return Math.PI / 2 - (grad * 2 * Math.PI / 400);
    }

    /* For ctx.arc: canvas native angle (clockwise from +x, y-down) */
    function gradToArc(grad) {
        return grad * 2 * Math.PI / 400 - Math.PI / 2;
    }

    /* ---- Draw sonar data for one angle ---- */

    function drawAngle(angle, data, numSamples, dataRange) {
        var dAngle = displayGrad(angle);

        /* Compass heading compensation: shift old data by heading change */
        if (compassEnabled && compassHeading !== null) {
            var entry = sonarStore[angle];
            if (entry && entry.heading !== null) {
                var headingDeltaGrad = Math.round((compassHeading - entry.heading) * 400 / 360);
                dAngle = (dAngle - headingDeltaGrad + 800) % 400;
            }
        }

        var angStep = 2 * Math.PI / 400;
        var overlap = 0.004; /* ~0.2deg — closes anti-alias seams */
        var startAng = gradToArc(dAngle) - angStep / 2 - overlap;
        var endAng = gradToArc(dAngle) + angStep / 2 + overlap;

        /* Clear the full wedge to remove stale data */
        offCtx.fillStyle = "#000000";
        offCtx.beginPath();
        offCtx.moveTo(cx, cy);
        offCtx.arc(cx, cy, radius, startAng, endAng);
        offCtx.closePath();
        offCtx.fill();

        /* Range-aware mapping: how stored data maps to current display */
        var rangeRatio = dataRange / config.range_mm;
        var visibleSamples, drawRadius;
        if (rangeRatio >= 1) {
            /* Data extends past display — clip samples, draw across full radius */
            visibleSamples = Math.ceil(numSamples / rangeRatio);
            drawRadius = radius;
        } else {
            /* All samples visible — draw in inner portion only */
            visibleSamples = numSamples;
            drawRadius = rangeRatio * radius;
        }
        var rStep = drawRadius / visibleSamples;

        /* Draw sample bins, batching consecutive same-intensity runs */
        var s = 0;
        while (s < visibleSamples) {
            var intensity = data[s];
            if (intensity === 0) { s++; continue; }
            var runStart = s;
            while (s < visibleSamples && data[s] === intensity) s++;

            var r0 = runStart * rStep;
            var r1 = s * rStep;

            offCtx.fillStyle = colorLUT[intensity];
            offCtx.beginPath();
            offCtx.arc(cx, cy, r1, startAng, endAng);
            if (r0 > 0) {
                offCtx.arc(cx, cy, r0, endAng, startAng, true);
            } else {
                offCtx.lineTo(cx, cy);
            }
            offCtx.closePath();
            offCtx.fill();
        }
        needsRedraw = true;
    }

    function storeAngle(angle, data, numSamples, rangeMm) {
        sonarStore[angle] = {
            data: new Uint8Array(data.slice(0, numSamples)),
            numSamples: numSamples,
            range_mm: rangeMm,
            heading: compassEnabled ? compassHeading : null
        };
        drawAngle(angle, sonarStore[angle].data, numSamples, rangeMm);
    }

    function angleInSector(angle) {
        var s = config.start_angle, e = config.end_angle;
        if (s <= e) return angle >= s && angle <= e;
        return angle >= s || angle <= e;
    }

    function redrawAll() {
        clearOffscreen();
        for (var i = 0; i < 400; i++) {
            var entry = sonarStore[i];
            if (entry && angleInSector(i)) drawAngle(i, entry.data, entry.numSamples, entry.range_mm);
        }
        needsRedraw = true;
    }

    /* ---- Spectrogram (Sound mode) ---- */

    function drawSoundColumn(data, numSamples) {
        /* Shift existing image left by 1px */
        soundCtx.drawImage(soundCanvas, -1, 0);
        /* Clear rightmost column */
        soundCtx.fillStyle = "#000000";
        soundCtx.fillRect(W - 1, 0, 1, H);

        /* Draw samples as vertical column at x = W-1 */
        /* Y=0 is near/surface, Y=H is far/deep */
        var imgData = soundCtx.getImageData(W - 1, 0, 1, H);
        var pixels = imgData.data;

        for (var y = 0; y < H; y++) {
            var sampleIdx = Math.floor((y / H) * numSamples);
            if (sampleIdx >= numSamples) sampleIdx = numSamples - 1;
            var intensity = data[sampleIdx];
            var pi = y * 4;
            var ci = intensity * 4;
            pixels[pi]     = currentPalette[ci];
            pixels[pi + 1] = currentPalette[ci + 1];
            pixels[pi + 2] = currentPalette[ci + 2];
            pixels[pi + 3] = 255;
        }
        soundCtx.putImageData(imgData, W - 1, 0);
        needsRedraw = true;
    }

    function drawSoundGrid() {
        ctx.strokeStyle = "rgba(88, 166, 255, 0.3)";
        ctx.lineWidth = 1;
        ctx.font = "11px monospace";
        ctx.fillStyle = "rgba(88, 166, 255, 0.6)";
        ctx.textAlign = "left";

        var rangeMm = config.range_mm;
        var rangeM = rangeMm / 1000;

        /* Auto-scale ring intervals (same logic as polar grid) */
        var ringInterval;
        if (rangeM <= 5) ringInterval = 1;
        else if (rangeM <= 10) ringInterval = 2;
        else if (rangeM <= 25) ringInterval = 5;
        else ringInterval = 10;

        var numRings = Math.floor(rangeM / ringInterval);

        /* Horizontal lines at range intervals */
        for (var i = 1; i <= numRings; i++) {
            var y = Math.round((i * ringInterval / rangeM) * H);
            ctx.beginPath();
            ctx.moveTo(0, y);
            ctx.lineTo(W, y);
            ctx.stroke();
            ctx.fillText(i * ringInterval + "m", 4, y - 3);
        }
    }

    /* ---- Grid overlay ---- */

    function drawGrid() {
        ctx.strokeStyle = "rgba(88, 166, 255, 0.3)";
        ctx.lineWidth = 1;
        ctx.font = "11px monospace";
        ctx.fillStyle = "rgba(88, 166, 255, 0.6)";
        ctx.textAlign = "center";

        var rangeMm = config.range_mm;
        var rangeM = rangeMm / 1000;

        /* Auto-scale ring intervals */
        var ringInterval;
        if (rangeM <= 5) ringInterval = 1;
        else if (rangeM <= 10) ringInterval = 2;
        else if (rangeM <= 25) ringInterval = 5;
        else ringInterval = 10;

        var numRings = Math.floor(rangeM / ringInterval);

        if (fullCircle) {
            /* Full circle: complete rings + 8 angle spokes */
            for (var i = 1; i <= numRings; i++) {
                var r = (i * ringInterval / rangeM) * radius;
                ctx.beginPath();
                ctx.arc(cx, cy, r, 0, 2 * Math.PI);
                ctx.stroke();
                ctx.fillText(i * ringInterval + "m", cx + 4, cy - r + 14);
            }

            if (!compassEnabled) {
                /* 8 fixed spokes at display positions (0°=forward always at top) */
                var angleLabels = ["0", "45", "90", "135", "180", "225", "270", "315"];
                for (var i = 0; i < 8; i++) {
                    var dGrad = i * 50;
                    var a = gradToRad(dGrad);
                    ctx.beginPath();
                    ctx.moveTo(cx, cy);
                    ctx.lineTo(cx + Math.cos(a) * radius, cy - Math.sin(a) * radius);
                    ctx.stroke();
                    var lx = cx + Math.cos(a) * (radius + 14);
                    var ly = cy - Math.sin(a) * (radius + 14);
                    ctx.fillText(angleLabels[i] + "\u00B0", lx, ly + 4);
                }
            }
        } else {
            /* Sector: draw arcs + boundary lines in display space */
            var dStart = displayGrad(config.start_angle);
            var dEnd = displayGrad(config.end_angle);
            if (mirrorDisplay) { var tmp = dStart; dStart = dEnd; dEnd = tmp; }

            var arcStart = gradToArc(dStart);
            var arcEnd = gradToArc(dEnd);

            for (var i = 1; i <= numRings; i++) {
                var r = (i * ringInterval / rangeM) * radius;
                ctx.beginPath();
                ctx.arc(cx, cy, r, arcStart, arcEnd, false);
                ctx.stroke();
            }

            /* Range labels along the forward center line */
            var fwdA = gradToRad(0);
            for (var i = 1; i <= numRings; i++) {
                var r = (i * ringInterval / rangeM) * radius;
                var lx = cx + Math.cos(fwdA) * r + 12;
                var ly = cy - Math.sin(fwdA) * r + 4;
                ctx.fillText(i * ringInterval + "m", lx, ly);
            }

            /* Sector boundary lines */
            var sA = gradToRad(dStart);
            var eA = gradToRad(dEnd);
            ctx.beginPath();
            ctx.moveTo(cx, cy);
            ctx.lineTo(cx + Math.cos(sA) * radius, cy - Math.sin(sA) * radius);
            ctx.stroke();
            ctx.beginPath();
            ctx.moveTo(cx, cy);
            ctx.lineTo(cx + Math.cos(eA) * radius, cy - Math.sin(eA) * radius);
            ctx.stroke();

            /* Center line (forward) */
            ctx.beginPath();
            ctx.moveTo(cx, cy);
            ctx.lineTo(cx + Math.cos(fwdA) * radius, cy - Math.sin(fwdA) * radius);
            ctx.stroke();
            ctx.fillText("0\u00B0", cx + Math.cos(fwdA) * (radius + 14),
                         cy - Math.sin(fwdA) * (radius + 14) + 4);
        }

        /* Center marker */
        ctx.fillStyle = "rgba(88, 166, 255, 0.8)";
        ctx.beginPath();
        ctx.arc(cx, cy, 3, 0, 2 * Math.PI);
        ctx.fill();
    }

    /* ---- Compass bearing ring ---- */

    function drawCompassRing() {
        if (!compassEnabled || compassHeading === null) return;

        var isSector = !fullCircle && config.start_angle !== config.end_angle;
        var dStart, dEnd;
        if (isSector) {
            dStart = displayGrad(config.start_angle);
            dEnd = displayGrad(config.end_angle);
            if (mirrorDisplay) { var tmp = dStart; dStart = dEnd; dEnd = tmp; }
        }

        var ringR = radius + 8;
        var labelR = radius + 22;
        var tickInner = radius + 4;

        /* Ring circle or sector arc */
        ctx.strokeStyle = "rgba(88, 166, 255, 0.4)";
        ctx.lineWidth = 1;
        ctx.beginPath();
        if (isSector) {
            ctx.arc(cx, cy, ringR, gradToArc(dStart), gradToArc(dEnd), false);
        } else {
            ctx.arc(cx, cy, ringR, 0, 2 * Math.PI);
        }
        ctx.stroke();

        /* Cardinal and intercardinal labels + ticks */
        var labels = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"];
        var bearings = [0, 45, 90, 135, 180, 225, 270, 315];

        ctx.font = "bold 11px monospace";
        ctx.textAlign = "center";
        ctx.textBaseline = "middle";

        for (var i = 0; i < bearings.length; i++) {
            /* Position relative to device forward (top):
               bearing B at heading H → display gradian = (-(B - H)) * 400/360 */
            var relDeg = bearings[i] - compassHeading;
            var dGrad = (relDeg * 400 / 360 + 4000) % 400;

            if (isSector && !dAngleInRange(dGrad, dStart, dEnd)) continue;

            var a = gradToRad(dGrad);
            var cosA = Math.cos(a);
            var sinA = Math.sin(a);

            /* Tick mark */
            var isCardinal = (i % 2 === 0);
            var tickOuter = ringR + (isCardinal ? 6 : 3);
            ctx.strokeStyle = isCardinal ? "rgba(88, 166, 255, 0.7)" : "rgba(88, 166, 255, 0.4)";
            ctx.lineWidth = isCardinal ? 2 : 1;
            ctx.beginPath();
            ctx.moveTo(cx + cosA * tickInner, cy - sinA * tickInner);
            ctx.lineTo(cx + cosA * tickOuter, cy - sinA * tickOuter);
            ctx.stroke();

            /* Label */
            var lx = cx + cosA * labelR;
            var ly = cy - sinA * labelR;
            ctx.fillStyle = (labels[i] === "N") ? "rgba(255, 100, 100, 0.9)" :
                            isCardinal ? "rgba(88, 166, 255, 0.8)" :
                            "rgba(88, 166, 255, 0.5)";
            ctx.fillText(labels[i], lx, ly);
        }

        /* 30-degree tick marks (every 30°, skip ones already drawn at 45° intervals) */
        ctx.strokeStyle = "rgba(88, 166, 255, 0.25)";
        ctx.lineWidth = 1;
        for (var b = 0; b < 360; b += 30) {
            if (b % 45 === 0) continue; /* already drawn above */
            var relDeg = b - compassHeading;
            var dGrad = (relDeg * 400 / 360 + 4000) % 400;
            if (isSector && !dAngleInRange(dGrad, dStart, dEnd)) continue;
            var a = gradToRad(dGrad);
            ctx.beginPath();
            ctx.moveTo(cx + Math.cos(a) * tickInner, cy - Math.sin(a) * tickInner);
            ctx.lineTo(cx + Math.cos(a) * (ringR + 2), cy - Math.sin(a) * (ringR + 2));
            ctx.stroke();
        }

        /* Forward indicator — small triangle at top */
        ctx.fillStyle = "rgba(88, 166, 255, 0.6)";
        var fwdA = gradToRad(0); /* 0 gradians = forward = top */
        var fx = cx + Math.cos(fwdA) * (ringR + 2);
        var fy = cy - Math.sin(fwdA) * (ringR + 2);
        ctx.beginPath();
        ctx.moveTo(fx, fy - 5);
        ctx.lineTo(fx - 3, fy + 2);
        ctx.lineTo(fx + 3, fy + 2);
        ctx.closePath();
        ctx.fill();

        /* Heading readout */
        ctx.font = "bold 12px monospace";
        ctx.fillStyle = "rgba(200, 204, 212, 0.7)";
        ctx.textAlign = "right";
        ctx.textBaseline = "top";
        var hdg = Math.round(compassHeading);
        var hdgStr = ("00" + hdg).slice(-3);
        ctx.fillText("HDG " + hdgStr + "\u00B0", W - 8, 8);
    }

    /* ---- Depth overlay on canvas ---- */

    function drawOverlay() {
        if (currentDepth === null) return;
        ctx.font = "bold 14px monospace";
        ctx.fillStyle = "rgba(200, 204, 212, 0.7)";
        ctx.textAlign = "left";
        ctx.textBaseline = "top";
        ctx.fillText("Depth: " + currentDepth.toFixed(2) + " m", 8, 8);
    }

    /* ---- Render loop ---- */

    function render() {
        if (needsRedraw) {
            ctx.fillStyle = "#0d1117";
            ctx.fillRect(0, 0, W, H);
            if (currentMode === "sound") {
                ctx.drawImage(soundCanvas, 0, 0);
                drawSoundGrid();
            } else {
                ctx.drawImage(offCanvas, 0, 0);
                drawGrid();
                drawCompassRing();
            }
            drawOverlay();
            needsRedraw = false;
        }
        requestAnimationFrame(render);
    }
    /* ---- WebSocket ---- */

    var ws = null;
    var reconnectTimer = null;

    function wsConnect() {
        var host = window.location.host || "192.168.4.1";
        var wsproto = (window.location.protocol === "https:") ? "wss://" : "ws://";
        ws = new WebSocket(wsproto + host + "/ws");
        ws.binaryType = "arraybuffer";

        ws.onopen = function () {
            document.getElementById("conn-indicator").className = "indicator connected";
            if (reconnectTimer) {
                clearTimeout(reconnectTimer);
                reconnectTimer = null;
            }
        };

        ws.onclose = function () {
            document.getElementById("conn-indicator").className = "indicator disconnected";
            ws = null;
            reconnectTimer = setTimeout(wsConnect, 2000);
        };

        ws.onerror = function () {
            if (ws) ws.close();
        };

        ws.onmessage = function (evt) {
            if (evt.data instanceof ArrayBuffer) {
                handleBinary(evt.data);
            } else {
                handleJSON(evt.data);
            }
        };
    }

    function handleBinary(buffer) {
        rxFrameCount++;
        var view = new DataView(buffer);
        var type = view.getUint8(0);
        if (type !== 0x01) return;

        var angle = view.getUint16(1, true);
        var numSamples = view.getUint16(3, true);
        var data = new Uint8Array(buffer, 5, numSamples);

        if (pendingClear) {
            sonarStore = new Array(400);
            clearOffscreen();
            pendingClear = false;
        }

        storeAngle(angle, data, numSamples, config.range_mm);
        if (currentMode === "sound") {
            drawSoundColumn(data, numSamples);
        }
    }

    function handleJSON(text) {
        var msg;
        try {
            msg = JSON.parse(text);
        } catch (e) {
            return;
        }

        if (msg.type === "status") {
            if (msg.depth_m !== undefined) currentDepth = msg.depth_m;
            document.getElementById("depth").textContent =
                msg.depth_m !== undefined ? msg.depth_m.toFixed(2) : "--";
            document.getElementById("battery").textContent =
                msg.batt_mv !== undefined ? (msg.batt_mv / 1000).toFixed(1) : "--";
            document.getElementById("scan-rate").textContent =
                msg.scan_rate !== undefined ? (msg.scan_rate * 0.9).toFixed(1) : "--";
            document.getElementById("ws-ok").textContent =
                msg.ws_ok !== undefined ? msg.ws_ok : "--";
            document.getElementById("ws-rx").textContent = rxFrameCount;
            rxFrameCount = 0;
            var dropTotal = (msg.ws_fail || 0) + (msg.q_fail || 0) + (msg.alloc_fail || 0);
            var dropEl = document.getElementById("ws-drop");
            dropEl.textContent = dropTotal;
            dropEl.style.color = dropTotal > 0 ? "#f85149" : "";
            needsRedraw = true; /* refresh overlay */
        } else if (msg.type === "config") {
            updateConfigUI(msg);
        }
    }

    function updateConfigUI(cfg) {
        var sectorChanged = false;
        if (cfg.gain !== undefined) {
            config.gain = cfg.gain;
            document.getElementById("gain").value = cfg.gain;
        }
        if (cfg.start_angle !== undefined) {
            if (config.start_angle !== cfg.start_angle) sectorChanged = true;
            config.start_angle = cfg.start_angle;
        }
        if (cfg.end_angle !== undefined) {
            if (config.end_angle !== cfg.end_angle) sectorChanged = true;
            config.end_angle = cfg.end_angle;
        }
        if (sectorChanged && currentMode !== "sound") {
            pendingClear = true;
            updateSectorRadio();
            updateViewport();
        }
        if (cfg.num_samples !== undefined) {
            config.num_samples = cfg.num_samples;
            updateSamplesRadio();
        }
        if (cfg.transmit_frequency !== undefined) {
            config.transmit_frequency = cfg.transmit_frequency;
        }
        if (cfg.transmit_duration !== undefined) {
            config.transmit_duration = cfg.transmit_duration;
            document.getElementById("tx-duration").textContent = cfg.transmit_duration + " \u00B5s";
        }
        if (cfg.sample_period !== undefined) {
            config.sample_period = cfg.sample_period;
            document.getElementById("sample-period").textContent = cfg.sample_period + " ticks";
        }
        if (cfg.range_mm !== undefined) {
            var rangeChanged = config.range_mm !== cfg.range_mm;
            config.range_mm = cfg.range_mm;
            var rangeM = Math.round(cfg.range_mm / 1000);
            document.getElementById("range").value = rangeM;
            document.getElementById("range-val").textContent = rangeM;
            if (rangeChanged) redrawAll();
        }
        if (cfg.speed_of_sound !== undefined) {
            config.speed_of_sound = cfg.speed_of_sound;
            document.getElementById("speed-of-sound").textContent = cfg.speed_of_sound + " m/s";
        }
        if (cfg.saltwater !== undefined) {
            config.saltwater = cfg.saltwater;
            var radios = document.querySelectorAll('input[name="water"]');
            for (var i = 0; i < radios.length; i++) {
                radios[i].checked = (radios[i].value === "salt") === cfg.saltwater;
            }
        }
    }

    /* ---- Controls ---- */

    var sendTimer = null;
    var pendingConfig = {};

    function queueConfigSend(key, value) {
        pendingConfig[key] = value;
        if (sendTimer) clearTimeout(sendTimer);
        sendTimer = setTimeout(function () {
            if (ws && ws.readyState === WebSocket.OPEN) {
                pendingConfig.cmd = "set_config";
                ws.send(JSON.stringify(pendingConfig));
            }
            pendingConfig = {};
            sendTimer = null;
        }, 200);
    }

    /* Range slider */
    var rangeEl = document.getElementById("range");
    var rangeVal = document.getElementById("range-val");
    rangeEl.addEventListener("input", function () {
        rangeVal.textContent = rangeEl.value;
        config.range_mm = parseInt(rangeEl.value) * 1000;
        queueConfigSend("range_mm", config.range_mm);
        redrawAll();
    });

    /* Gain */
    document.getElementById("gain").addEventListener("change", function () {
        config.gain = parseInt(this.value);
        queueConfigSend("gain", config.gain);
    });

    /* Sector angle presets — symmetric about 0deg (gradian 0) */
    var sectorPresets = {
        "10":  { start: 394, end: 6 },
        "25":  { start: 386, end: 14 },
        "90":  { start: 350, end: 50 },
        "120": { start: 333, end: 67 },
        "360": { start: 0, end: 399 }
    };

    function updateSectorRadio() {
        var radios = document.querySelectorAll('input[name="sector"]');
        var matched = false;
        for (var i = 0; i < radios.length; i++) {
            var p = sectorPresets[radios[i].value];
            if (!p) continue;
            var expStart, expEnd;
            if (radios[i].value === "360") {
                expStart = 0;
                expEnd = 399;
            } else {
                expStart = (p.start + offsetGrad) % 400;
                expEnd = (p.end + offsetGrad) % 400;
            }
            if (config.start_angle === expStart && config.end_angle === expEnd) {
                radios[i].checked = true;
                matched = true;
            }
        }
        if (!matched) {
            for (var i = 0; i < radios.length; i++) radios[i].checked = false;
        }
    }

    var sectorRadios = document.querySelectorAll('input[name="sector"]');
    for (var i = 0; i < sectorRadios.length; i++) {
        sectorRadios[i].addEventListener("change", function () {
            var preset = sectorPresets[this.value];
            if (!preset) return;
            if (this.value === "360") {
                config.start_angle = 0;
                config.end_angle = 399;
            } else {
                config.start_angle = (preset.start + offsetGrad) % 400;
                config.end_angle = (preset.end + offsetGrad) % 400;
            }
            pendingClear = true;
            updateViewport();
            queueConfigSend("start_angle", config.start_angle);
            queueConfigSend("end_angle", config.end_angle);
        });
    }

    /* Num samples radio buttons */
    function updateSamplesRadio() {
        var radios = document.querySelectorAll('input[name="samples"]');
        var matched = false;
        for (var i = 0; i < radios.length; i++) {
            if (parseInt(radios[i].value) === config.num_samples) {
                radios[i].checked = true;
                matched = true;
            }
        }
        if (!matched) {
            for (var i = 0; i < radios.length; i++) radios[i].checked = false;
        }
    }

    var samplesRadios = document.querySelectorAll('input[name="samples"]');
    for (var i = 0; i < samplesRadios.length; i++) {
        samplesRadios[i].addEventListener("change", function () {
            config.num_samples = parseInt(this.value);
            queueConfigSend("num_samples", config.num_samples);
        });
    }

    /* Water type (salt/fresh) */
    var waterRadios = document.querySelectorAll('input[name="water"]');
    for (var i = 0; i < waterRadios.length; i++) {
        waterRadios[i].addEventListener("change", function () {
            config.saltwater = (this.value === "salt");
            queueConfigSend("saltwater", config.saltwater);
        });
    }

    /* Display offset — adjusts sonar scan angles to compensate for mounting */
    document.getElementById("offset-angle").addEventListener("change", function () {
        var newOffset = parseInt(this.value);
        var delta = (newOffset - offsetGrad + 400) % 400;
        offsetGrad = newOffset;
        if (currentMode === "sound") {
            /* Update sound mode lock angle to vessel forward */
            savedSector.start = (savedSector.start + delta) % 400;
            savedSector.end = (savedSector.end + delta) % 400;
            config.start_angle = newOffset;
            config.end_angle = newOffset;
            queueConfigSend("start_angle", newOffset);
            queueConfigSend("end_angle", newOffset);
        } else if (!fullCircle) {
            /* Adjust sector scan angles by the offset change */
            config.start_angle = (config.start_angle + delta) % 400;
            config.end_angle = (config.end_angle + delta) % 400;
            pendingClear = true;
            queueConfigSend("start_angle", config.start_angle);
            queueConfigSend("end_angle", config.end_angle);
        }
        updateViewport();
    });

    /* Mirror display (client-side only) */
    document.getElementById("mirror-display").addEventListener("change", function () {
        mirrorDisplay = this.checked;
        updateViewport();
    });

    /* ---- Compass bearing ring ---- */

    var compassListenerAttached = false;

    function onOrientationEvent(evt) {
        var heading = null;
        if (evt.webkitCompassHeading !== undefined) {
            /* iOS Safari: webkitCompassHeading is degrees from north */
            heading = evt.webkitCompassHeading;
        } else if (evt.alpha !== null) {
            /* Absolute orientation: alpha is rotation around z-axis */
            heading = (360 - evt.alpha) % 360;
        }
        if (heading === null) return;

        compassHeading = heading;

        /* Quantized redraw: only redraw when heading changes by > 1 gradian (~0.9°) */
        if (lastRedrawHeading === null) {
            lastRedrawHeading = heading;
            redrawAll();
        } else {
            var deltaDeg = Math.abs(heading - lastRedrawHeading);
            if (deltaDeg > 180) deltaDeg = 360 - deltaDeg;
            if (deltaDeg >= 0.9) {
                lastRedrawHeading = heading;
                redrawAll();
            } else {
                needsRedraw = true; /* at least refresh the compass ring */
            }
        }
    }

    function compassInit() {
        if (compassListenerAttached) return;

        /* iOS 13+ requires permission request from user gesture */
        if (typeof DeviceOrientationEvent !== "undefined" &&
            typeof DeviceOrientationEvent.requestPermission === "function") {
            DeviceOrientationEvent.requestPermission().then(function (state) {
                if (state === "granted") {
                    window.addEventListener("deviceorientation", onOrientationEvent);
                    compassListenerAttached = true;
                }
            }).catch(function () { /* permission denied or error */ });
        } else {
            /* Android Chrome: prefer absolute orientation */
            if ("ondeviceorientationabsolute" in window) {
                window.addEventListener("deviceorientationabsolute", onOrientationEvent);
            } else {
                window.addEventListener("deviceorientation", onOrientationEvent);
            }
            compassListenerAttached = true;
        }
    }

    function compassDeinit() {
        if (!compassListenerAttached) return;
        window.removeEventListener("deviceorientationabsolute", onOrientationEvent);
        window.removeEventListener("deviceorientation", onOrientationEvent);
        compassListenerAttached = false;
    }

    document.getElementById("compass-toggle").addEventListener("change", function () {
        compassEnabled = this.checked;
        if (compassEnabled) {
            compassInit();
        } else {
            compassDeinit();
            compassHeading = null;
            lastRedrawHeading = null;
        }
        computeViewport();
        redrawAll();
    });

    /* Mode switching (Scan / Sound) */
    var modeRadios = document.querySelectorAll('input[name="mode"]');
    for (var i = 0; i < modeRadios.length; i++) {
        modeRadios[i].addEventListener("change", function () {
            if (this.value === currentMode) return;
            if (this.value === "sound") {
                /* SCAN -> SOUND */
                savedSector.start = config.start_angle;
                savedSector.end = config.end_angle;
                currentMode = "sound";
                clearSoundCanvas();
                appEl.classList.add("sound-mode");
                /* Lock sonar to vessel forward (offset-adjusted) */
                config.start_angle = offsetGrad;
                config.end_angle = offsetGrad;
                queueConfigSend("start_angle", offsetGrad);
                queueConfigSend("end_angle", offsetGrad);
            } else {
                /* SOUND -> SCAN */
                currentMode = "scan";
                appEl.classList.remove("sound-mode");
                /* Restore saved sector */
                config.start_angle = savedSector.start;
                config.end_angle = savedSector.end;
                queueConfigSend("start_angle", savedSector.start);
                queueConfigSend("end_angle", savedSector.end);
                updateSectorRadio();
                updateViewport();
            }
            needsRedraw = true;
        });
    }

    /* Color palette */
    document.getElementById("palette").addEventListener("change", function () {
        currentPalette = palettes[this.value] || palettes.grayscale;
        rebuildColorLUT();
        if (currentMode === "sound") {
            /* Can't recolor existing spectrogram — just clear and continue */
            clearSoundCanvas();
        }
        redrawAll();
    });

    /* ---- Reset buttons ---- */

    document.getElementById("reset-sonar").addEventListener("click", function () {
        if (ws && ws.readyState === WebSocket.OPEN) {
            ws.send(JSON.stringify({ cmd: "reset_sonar" }));
        }
    });

    document.getElementById("reset-controller").addEventListener("click", function () {
        if (ws && ws.readyState === WebSocket.OPEN) {
            ws.send(JSON.stringify({ cmd: "reset_controller" }));
        }
    });

    /* ---- Test pattern (Cartesian checkerboard) ---- */

    document.getElementById("test-pattern").addEventListener("click", function () {
        var numSamples = config.num_samples;

        if (currentMode === "sound") {
            /* In sound mode, draw a gradient test column */
            var data = new Uint8Array(numSamples);
            for (var s = 0; s < numSamples; s++) {
                data[s] = Math.round((s / numSamples) * 255);
            }
            drawSoundColumn(data, numSamples);
            return;
        }

        var start = config.start_angle;
        var end = config.end_angle;
        var isFull = (start === 0 && end === 399);
        var sectorSize = isFull ? 400 : ((end - start + 400) % 400 + 1);

        /* Uniform square grid in rendered pixel space */
        var numCells = 8;
        var cellPx = (2 * radius) / numCells;
        var totalCells = numCells * numCells;

        for (var step = 0; step < sectorSize; step++) {
            var angle = isFull ? step : ((start + step) % 400);
            var angRad = gradToRad(angle);
            var cosA = Math.cos(angRad);
            var sinA = Math.sin(angRad);
            var data = new Uint8Array(numSamples);

            for (var s = 0; s < numSamples; s++) {
                /* Cartesian offset from origin in pixels */
                var rFrac = s / numSamples;
                var dx = cosA * rFrac * radius;
                var dy = -sinA * rFrac * radius;

                /* Map to grid cell */
                var gx = Math.floor((dx + radius) / cellPx);
                var gy = Math.floor((dy + radius) / cellPx);
                if (gx < 0) gx = 0;
                if (gx >= numCells) gx = numCells - 1;
                if (gy < 0) gy = 0;
                if (gy >= numCells) gy = numCells - 1;

                if ((gx + gy) % 2 === 0) {
                    var cellIdx = gy * numCells + gx;
                    data[s] = 20 + Math.round(cellIdx * 220 / (totalCells - 1));
                } else {
                    data[s] = 0;
                }
            }
            storeAngle(angle, data, numSamples, config.range_mm);
        }
    });

    /* ---- Menu toggle ---- */

    var menuBtn = document.getElementById("menu-toggle");
    var appEl = document.querySelector(".app");

    /* Start collapsed on small screens */
    if (window.innerWidth <= 700) {
        appEl.classList.add("menu-hidden");
    }

    menuBtn.addEventListener("click", function () {
        appEl.classList.toggle("menu-hidden");
        /* Let the CSS transition finish, then resize the canvas */
        setTimeout(resizeCanvas, 260);
    });

    /* ---- Start ---- */

    resizeCanvas();
    requestAnimationFrame(render);

    var resizeTimer = null;
    window.addEventListener("resize", function () {
        if (resizeTimer) clearTimeout(resizeTimer);
        resizeTimer = setTimeout(resizeCanvas, 100);
    });

    wsConnect();

})();
