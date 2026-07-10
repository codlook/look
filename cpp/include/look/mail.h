#pragma once
#include "interpreter.h"

namespace look {

// mail:: module — zero-dependency email via HTTP API providers
// Supports: Mailgun, SendGrid, SMTP2Go, Postmark (same API shape)
// Provider configured via env: MAIL_PROVIDER, MAIL_API_KEY, MAIL_FROM
// use mail;

Module make_mail_module();

} // namespace look
