#include "zone/client.h"

// akk-stack: limitless paged personal bank. Temporary manual trigger for the M1 server backend
// (the M2 client bank window will drive Client::SwapBankPage via OP_BankPageSwap instead).
// Usage: #bankpage [N|next|prev]. Must be near a banker (SwapBankPage enforces that).
void command_bankpage(Client *c, const Seperator *sep)
{
	const int cur = c->GetBankPage();
	int       target;

	if (!strcasecmp(sep->arg[1], "next")) {
		target = cur + 1;
	} else if (!strcasecmp(sep->arg[1], "prev")) {
		target = cur - 1;
	} else if (sep->IsNumber(1)) {
		target = Strings::ToInt(sep->arg[1]);
	} else {
		c->Message(
			Chat::White,
			fmt::format(
				"Bank page [{}] of [{}]. Usage: #bankpage [N|next|prev].",
				c->GetBankPage(),
				c->GetBankPageCount()
			).c_str()
		);
		return;
	}

	c->SwapBankPage(target);
}
