#include "litehtml/html.h"
#include "litehtml/el_script.h"
#include "litehtml/document.h"


litehtml::el_script::el_script(const std::shared_ptr<litehtml::document>& doc) : litehtml::element(doc)
{

}

litehtml::el_script::~el_script()
{

}

void litehtml::el_script::parse_attributes()
{
	//TODO: pass script text to document container
}

bool litehtml::el_script::appendChild(const ptr &el)
{
	el->get_text(m_text);
	return true;
}

const litehtml::tchar_t* litehtml::el_script::get_tagName() const
{
	return _t("script");
}
