'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const aim = require('../aim_math.js');
const config = { fovDegrees: 10, maxDistance: 200 };

test('accepts a target along the shot and returns a unit direction', () => {
    const [result] = aim.rank([0, 0, 0], [0, 0, 4], [[0, 0, 20]], config);
    assert.deepEqual(result.direction, [0, 0, 1]);
    assert.equal(result.distance, 20);
});
test('rejects targets behind the shooter and outside the full cone', () => {
    assert.equal(aim.rank([0, 0, 0], [0, 0, 1], [[0, 0, -20], [4, 0, 20]], config).length, 0);
});
test('uses the configured full angle, rather than twice its intended width', () => {
    const at = degrees => [Math.sin(degrees * Math.PI / 180) * 20, 0, Math.cos(degrees * Math.PI / 180) * 20];
    assert.equal(aim.rank([0, 0, 0], [0, 0, 1], [at(4.9), at(5.1)], config).length, 1);
});
test('ranks by aim angle before distance', () => {
    const ranked = aim.rank([0, 0, 0], [0, 0, 1], [[0.1, 0, 5], [0, 0, 100]], config);
    assert.deepEqual(ranked[0].point, [0, 0, 100]);
});
test('chooses closer target when angular positions are equal', () => {
    const ranked = aim.rank([0, 0, 0], [0, 0, 1], [[0, 0, 100], [0, 0, 20]], config);
    assert.equal(ranked[0].distance, 20);
});
test('omits coincident, near-zero, nonfinite, and excessive-distance points', () => {
    const ranked = aim.rank([0, 0, 0], [0, 0, 1], [[0, 0, 0], [0, 0, 0.1], [NaN, 0, 20], [0, 0, 201]], config);
    assert.equal(ranked.length, 0);
});
test('does not generate a direction when incoming shot direction is invalid', () => {
    for (const direction of [[0, 0, 0], [Infinity, 0, 1], [0, 1]])
        assert.equal(aim.rank([0, 0, 0], direction, [[0, 0, 20]], config).length, 0);
});
test('accounts for a displaced shot origin', () => {
    const [target] = aim.rank([10, 5, 10], [0, 0, 1], [[10, 5, 30]], config);
    assert.deepEqual(target.direction, [0, 0, 1]);
    assert.equal(target.distance, 20);
});
test('does not mutate candidate coordinates', () => {
    const points = [[0, 0, 20]];
    aim.rank([0, 0, 0], [0, 0, 1], points, config);
    assert.deepEqual(points, [[0, 0, 20]]);
});
