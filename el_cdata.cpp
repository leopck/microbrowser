#include "litehtml/html.h"
#include "litehtml/el_cdata.h"

litehtml::el_cdata::el_cdata(const std::shared_ptr<litehtml::document>& doc) : litehtml::element(doc)
{
	m_skip = true;
}

litehtml::el_cdata::~el_cdata()
{

}

void litehtml::el_cdata::get_text( tstring& text )
{
	text += m_text;
}

void litehtml::el_cdata::set_data( const tchar_t* data )
{
	if(data)
	{
		m_text += data;
	}
}
