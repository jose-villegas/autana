"""Indexed reads with an order-independent fixed-point gradient reduction."""

import torch


# Headroom bounds the absolute sum, including rounding, below signed int64 overflow.
GRADIENT_SUM_LIMIT = 2 ** 61


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
        maximum = gradient.abs().amax().double()
        if not torch.isfinite(maximum).item():
            raise ValueError("gather backward requires finite gradients")
        scale = (GRADIENT_SUM_LIMIT / max(index.numel(), 1)) / torch.where(maximum > 0, maximum, 1)
        fixed = (gradient.double() * scale).round().to(torch.int64)
        deterministic = torch.are_deterministic_algorithms_enabled()
        warn_only = torch.is_deterministic_algorithms_warn_only_enabled()
        try:
            # Integer sums commute exactly; avoid torch's serial sorted float reduction path.
            torch.use_deterministic_algorithms(False)
            result.index_add_(0, index.reshape(-1), fixed.reshape(-1, *ctx.source_shape[1:]))
        finally:
            torch.use_deterministic_algorithms(deterministic, warn_only=warn_only)
        return (result.double() / scale).to(gradient.dtype), None


gather = FixedPointGather.apply
