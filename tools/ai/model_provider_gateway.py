from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class ProviderPolicy:
    design_providers: tuple[str, ...] = ("gpt", "image2")
    geometry_provider: str = "tripo"
    automatic_fallback: bool = False
    max_paid_model_tasks_per_confirmation: int = 1


def provider_policy() -> ProviderPolicy:
    return ProviderPolicy()


class ProviderGatewayError(RuntimeError):
    def __init__(
        self,
        message: str,
        *,
        code: str,
        category: str,
        provider: str = "",
        operation: str = "",
        retryable: bool = False,
        ambiguous: bool = False,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.category = category
        self.provider = provider
        self.operation = operation
        self.retryable = retryable
        self.ambiguous = ambiguous


class PaidTaskAuthorization:
    def __init__(self, request_id: str, provider: str, operation: str) -> None:
        self.request_id = request_id
        self.provider = provider
        self.operation = operation
        self._consumed = False

    @classmethod
    def confirmed(cls, request_id: str) -> PaidTaskAuthorization:
        normalized = request_id.strip() if isinstance(request_id, str) else ""
        if not normalized:
            raise ProviderGatewayError(
                "A paid task authorization requires a request ID.",
                code="invalid_authorization",
                category="authorization",
            )
        return cls(normalized, "tripo", "model_generation")

    def consume(self, provider: str, operation: str) -> None:
        if provider != self.provider or operation != self.operation:
            raise ProviderGatewayError(
                "The paid task authorization does not cover this provider operation.",
                code="authorization_scope_mismatch",
                category="authorization",
                provider=provider,
                operation=operation,
            )
        if self._consumed:
            raise ProviderGatewayError(
                "The paid task authorization has already been consumed.",
                code="authorization_consumed",
                category="authorization",
                provider=provider,
                operation=operation,
            )
        self._consumed = True
