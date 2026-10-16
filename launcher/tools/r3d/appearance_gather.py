"""Indexed reads with an order-independent fixed-point gradient reduction."""

import torch


# Headroom bounds the absolute sum, including rounding, below signed int64 overflow.
GRADIENT_SUM_LIMIT = 2 ** 61
# Keep the power-of-two factor finite in float32.
GRADIENT_SCALE_EXPONENT_LIMIT = 126


class FixedPointGather(torch.autograd.Function):
    @staticmethod
    def forward(ctx, source, index):
        ctx.save_for_backward(index)
        ctx.source_shape = source.shape
        return source[index]

    @staticmethod
    def backward(ctx, gradient):
        index, = ctx.saved_tensors
        result = torch.zeros(ctx.source_shape, dtype=torch.int64, device=gradient.device)
        if not gradient.numel():
            return result.to(gradient.dtype), None
        maximum = gradient.abs().amax()
        finite = torch.isfinite(maximum)
        _, exponent = torch.frexp(maximum)
        headroom = (GRADIENT_SUM_LIMIT // max(index.numel(), 1)).bit_length() - 1
        exponent = (headroom - exponent).clamp(-GRADIENT_SCALE_EXPONENT_LIMIT, GRADIENT_SCALE_EXPONENT_LIMIT)
        scale = torch.exp2(exponent.to(gradient.dtype))
        fixed = (gradient * scale).round().to(torch.int64)
        deterministic = torch.are_deterministic_algorithms_enabled()
        warn_only = torch.is_deterministic_algorithms_warn_only_enabled()
        try:
            # Integer sums commute exactly; avoid torch's serial sorted float reduction path.
            torch.use_deterministic_algorithms(False)
            result.index_add_(0, index.reshape(-1), fixed.reshape(-1, *ctx.source_shape[1:]))
        finally:
            torch.use_deterministic_algorithms(deterministic, warn_only=warn_only)
        reduced = result.to(gradient.dtype) / scale
        return torch.where(finite, reduced, float("nan")), None


gather = FixedPointGather.apply
