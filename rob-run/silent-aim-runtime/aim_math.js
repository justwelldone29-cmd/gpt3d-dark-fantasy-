'use strict';

const AimMath = (() => {
    const valid = v => v && v.length === 3 && v.every(Number.isFinite);
    const subtract = (a, b) => a.map((v, i) => v - b[i]);
    const length = v => Math.hypot(...v);
    const normalize = v => {
        if (!valid(v)) return null;
        const n = length(v);
        return n > 1e-6 ? v.map(x => x / n) : null;
    };
    function rank(origin, forward, points, config) {
        const direction = normalize(forward);
        if (!valid(origin) || !direction) return [];
        const limit = Math.cos(config.fovDegrees * Math.PI / 360);
        return points.filter(valid).map(point => {
            const delta = subtract(point, origin);
            const distance = length(delta);
            const unit = normalize(delta);
            return unit ? { point, distance, direction: unit,
                dot: direction.reduce((sum, x, i) => sum + x * unit[i], 0) } : null;
        }).filter(x => x && x.distance >= 0.5 && x.distance <= config.maxDistance && x.dot >= limit)
            .sort((a, b) => b.dot - a.dot || a.distance - b.distance);
    }
    return { valid, subtract, length, normalize, rank };
})();

if (typeof module !== 'undefined') module.exports = AimMath;
