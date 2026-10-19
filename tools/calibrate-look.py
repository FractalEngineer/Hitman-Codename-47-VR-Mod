"""Measure how far the engine's camera actually turned between two captures.

The training level's floor is a tiled plane, and a plane's image is a homography
whatever the depth. The player turning the camera orbits it, so the frames are
not a pure rotation - but restricting the fit to the floor plane still recovers
the rotation, which is what `decomposeHomographyMat` returns along with the
translation. The solution closest to a pure yaw is the one to keep.

Usage:
    python tools/calibrate-look.py <before.png> <after.png> --px N [--fov DEG]

--px is how many pixels of mouse delta were injected between the two captures.
The printed degrees-per-pixel is what the look driver's `degperpx` should be.
"""

import argparse
import math
import sys

import cv2
import numpy as np


def load_floor(path, region):
    image = cv2.imread(path, cv2.IMREAD_GRAYSCALE)
    if image is None:
        sys.exit("cannot read " + path)
    # The HUD is screen-space and would bias the fit.
    image[:60, :] = 0
    if region is not None:
        # Only the floor is a plane. The player, the furniture and the walls are
        # not, and a handful of their features is enough to bend the fit.
        x0, y0, x1, y1 = region
        mask = np.zeros_like(image)
        mask[y0:y1, x0:x1] = 255
        image = cv2.bitwise_and(image, mask)
    return image


def match(alpha, beta):
    sift = cv2.SIFT_create()
    key_a, desc_a = sift.detectAndCompute(alpha, None)
    key_b, desc_b = sift.detectAndCompute(beta, None)
    if desc_a is None or desc_b is None or len(key_a) < 12 or len(key_b) < 12:
        sys.exit("not enough features - is the floor visible?")

    matcher = cv2.BFMatcher(cv2.NORM_L2)
    pairs = matcher.knnMatch(desc_a, desc_b, k=2)
    good = [a for a, b in pairs if a.distance < 0.75 * b.distance]
    if len(good) < 12:
        sys.exit("only %d reliable matches" % len(good))

    src = np.float32([key_a[m.queryIdx].pt for m in good])
    dst = np.float32([key_b[m.trainIdx].pt for m in good])
    homography, mask = cv2.findHomography(src, dst, cv2.RANSAC, 3.0)
    if homography is None:
        sys.exit("homography failed")
    inliers = int(mask.sum()) if mask is not None else 0
    return homography, len(good), inliers


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("before")
    parser.add_argument("after")
    parser.add_argument("--px", type=float, required=True,
                        help="injected mouse pixels between the two captures")
    parser.add_argument("--fov", type=float, default=34.52,
                        help="vertical field of view in degrees (glFrustum says 34.52)")
    parser.add_argument("--region", default=None,
                        help="x0,y0,x1,y1 rectangle that contains only floor")
    args = parser.parse_args()

    region = None
    if args.region:
        region = tuple(int(v) for v in args.region.split(","))
        if len(region) != 4:
            sys.exit("--region wants x0,y0,x1,y1")

    alpha = load_floor(args.before, region)
    beta = load_floor(args.after, region)
    height, width = alpha.shape

    homography, matched, inliers = match(alpha, beta)
    if inliers < 20:
        sys.exit("only %d inliers of %d matches - not a reliable fit"
                 % (inliers, matched))

    # The engine's projection is a plain perspective from glFrustum: square
    # pixels, principal point in the middle.
    focal = (height / 2.0) / math.tan(math.radians(args.fov) / 2.0)
    camera_matrix = np.array([
        [focal, 0.0, width / 2.0],
        [0.0, focal, height / 2.0],
        [0.0, 0.0, 1.0],
    ])

    count, rotations, translations, normals = cv2.decomposeHomographyMat(
        homography, camera_matrix)

    print("matches %d, inliers %d, focal %.1f px" % (matched, inliers, focal))
    best = None
    for index in range(count):
        rotation = rotations[index]
        translation = translations[index].ravel()
        # Anything with roll or pitch is not the yaw we injected.
        off_axis = abs(rotation[1, 0]) + abs(rotation[1, 2]) + abs(rotation[2, 1])
        angle = math.degrees(math.atan2(rotation[0, 2], rotation[0, 0]))
        print("  solution %d: yaw %+8.3f deg, off-axis %.4f, |t| %.3f"
              % (index, angle, off_axis, np.linalg.norm(translation)))
        if best is None or off_axis < best[0]:
            best = (off_axis, angle)

    angle = best[1]
    print()
    print("chosen yaw: %+.3f deg for %+.0f px" % (angle, args.px))
    print("degrees per pixel: %+.5f" % (angle / args.px))


if __name__ == "__main__":
    main()
