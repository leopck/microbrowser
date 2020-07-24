#include "litehtml/html.h"
#include "litehtml/context.h"
#include "litehtml/stylesheet.h"


void litehtml::context::load_master_stylesheet( const tchar_t* str )
{
	media_query_list::ptr media;

	m_master_css.parse_stylesheet(str, 0, std::shared_ptr<litehtml::document>(), media_query_list::ptr());
	m_master_css.sort_selectors();
}
