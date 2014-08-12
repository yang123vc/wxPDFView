/////////////////////////////////////////////////////////////////////////////
// Name:        src/PDFViewPages.cpp
// Purpose:     wxPDFViewPages implementation
// Author:      Tobias Taschner
// Created:     2014-08-07
// Copyright:   (c) 2014 Tobias Taschner
// Licence:     wxWindows licence
/////////////////////////////////////////////////////////////////////////////

#include "private/PDFViewPages.h"

#include <wx/rawbmp.h>

#include "fpdftext.h"

#include <algorithm>

//
// wxPDFViewPage
// 

wxPDFViewPage::wxPDFViewPage(wxPDFViewPages* pages, int index):
	m_pages(pages),
	m_index(index),
	m_page(NULL),
	m_textPage(NULL)
{

}

wxPDFViewPage::~wxPDFViewPage()
{
	Unload();
}

void wxPDFViewPage::Unload()
{
	if (m_page)
	{
		FPDF_ClosePage(m_page);
		m_page = NULL;
	}
	if (m_textPage)
	{
		FPDFText_ClosePage(m_textPage);
		m_textPage = NULL;
	}
}

FPDF_PAGE wxPDFViewPage::GetPage()
{
	if (!m_page)
	{
		m_page = FPDF_LoadPage(m_pages->doc(), m_index);
	}
	return m_page;
}

FPDF_TEXTPAGE wxPDFViewPage::GetTextPage()
{
	if (!m_textPage)
	{
		m_textPage = FPDFText_LoadPage(GetPage());
	}

	return m_textPage;
}

wxRect wxPDFViewPage::PageToScreen(const wxRect& pageRect, double left, double top, double right, double bottom)
{

	int screenLeft, screenTop, screenRight, screenBottom;
	FPDF_PageToDevice(GetPage(), 0, 0, pageRect.width, pageRect.height, 0, left, top, &screenLeft, &screenTop);
	FPDF_PageToDevice(GetPage(), 0, 0, pageRect.width, pageRect.height, 0, right, bottom, &screenRight, &screenBottom);

	return wxRect(screenLeft, screenTop, screenRight - screenLeft + 1, screenBottom - screenTop + 1);
}

void wxPDFViewPage::Draw(wxPDFViewPagesClient* client, wxDC& dc, wxGraphicsContext& gc, const wxRect& rect)
{
	// Draw page background
	wxRect bgRect = rect.Inflate(2, 2);
	gc.SetBrush(*wxWHITE_BRUSH);
	gc.SetPen(*wxBLACK_PEN);
	gc.DrawRectangle(bgRect.x, bgRect.y, bgRect.width, bgRect.height);

	// Draw any size bitmap regardless of size (blurry page is better than empty page)
	// Calculate the required bitmap size
	wxSize bmpSize = rect.GetSize();
	double scaleX, scaleY;
	dc.GetUserScale(&scaleX, &scaleY);
	bmpSize.x *= scaleX;
	bmpSize.y *= scaleY;

	wxBitmap bmp = client->GetCachedBitmap(m_index, bmpSize);
	if (bmp.IsOk())
		gc.DrawBitmap(bmp, rect.x, rect.y, rect.width, rect.height);
}

void wxPDFViewPage::DrawThumbnail(wxPDFViewPagesClient* client, wxDC& dc, const wxRect& rect)
{
	dc.SetBackground(*wxTRANSPARENT_BRUSH);
	dc.SetPen(*wxLIGHT_GREY_PEN);
	dc.DrawRectangle(rect.Inflate(1, 1));

	wxBitmap bmp = client->GetCachedBitmap(m_index, rect.GetSize());
	if (bmp.Ok())
	{
		dc.DrawBitmap(bmp, rect.GetPosition());
	}
}

void wxPDFViewPage::DrawPrint(wxDC& dc, const wxRect& rect, bool forceBitmap)
{
	FPDF_PAGE page = GetPage();

	int renderFlags = FPDF_ANNOT | FPDF_PRINTING;
#ifdef wxPDFVIEW_USE_RENDER_TO_DC
	if (!forceBitmap)
	{
		FPDF_RenderPage(dc.GetHDC(), page, rect.x, rect.y, rect.width, rect.height, 0, renderFlags);
	} else
#endif
	{
		wxBitmap bmp = CreateBitmap(page, rect.GetSize(), renderFlags);
		wxMemoryDC memDC(bmp);
		dc.Blit(rect.GetPosition(), rect.GetSize(), &memDC, wxPoint(0, 0));
	}
}

wxBitmap wxPDFViewPage::CreateCacheBitmap(const wxSize& bmpSize)
{
	// Render to bitmap
	FPDF_PAGE page = GetPage();

#ifdef wxPDFVIEW_USE_RENDER_TO_DC
	wxBitmap bmp(bmpSize, 24);
	wxMemoryDC memDC(bmp);
	memDC.SetBackground(*wxWHITE_BRUSH);
	memDC.Clear();
	FPDF_RenderPage(memDC.GetHDC(), page, 0, 0, bmpSize.x, bmpSize.y, 0, FPDF_LCD_TEXT);
	return bmp;
#else
	return CreateBitmap(page, bmpSize, FPDF_LCD_TEXT);
#endif
}

wxBitmap wxPDFViewPage::CreateBitmap(FPDF_PAGE page, const wxSize& bmpSize, int flags)
{
	FPDF_BITMAP bitmap = FPDFBitmap_Create(bmpSize.x, bmpSize.y, 0);
	FPDFBitmap_FillRect(bitmap, 0, 0, bmpSize.x, bmpSize.y, 0xFFFFFFFF);

	FPDF_RenderPageBitmap(bitmap, page, 0, 0, bmpSize.x, bmpSize.y, 0, flags);
	unsigned char* buffer =
		reinterpret_cast<unsigned char*>(FPDFBitmap_GetBuffer(bitmap));

	// Convert BGRA image data from PDF SDK to RGB image data
	wxBitmap bmp(bmpSize, 24);
	unsigned char* srcP = buffer;
	wxNativePixelData data(bmp);
	wxNativePixelData::Iterator p(data);
	for (int y = 0; y < bmpSize.y; ++y)
	{
		wxNativePixelData::Iterator rowStart = p;

		for (int x = 0; x < bmpSize.x; ++x, ++p)
		{
			p.Blue() = *(srcP++);
			p.Green() = *(srcP++);
			p.Red() = *(srcP++);
			srcP++;
		}

		p = rowStart;
		p.OffsetY(data, 1);
	}

	FPDFBitmap_Destroy(bitmap);

	return bmp;
}

//
// wxPDFViewPages
//

wxPDFViewPages::wxPDFViewPages():
	m_doc(NULL)
{
	// Init bitmap request handler
	m_bmpUpdateHandlerActive = true;
	m_bmpUpdateHandlerCondition = NULL;

	if (CreateThread(wxTHREAD_JOINABLE) != wxTHREAD_NO_ERROR)     
	{         
		wxLogError("Could not create the worker thread!");         
		return;     
	}

	if (GetThread()->Run() != wxTHREAD_NO_ERROR)
	{
		wxLogError("Could not run the worker thread!");
		return;
	}
}

wxPDFViewPages::~wxPDFViewPages()
{
	if (m_bmpUpdateHandlerCondition)
	{
		// Finish bitmap update handler
		m_bmpUpdateHandlerActive = false;
		m_bmpUpdateHandlerCondition->Signal();
		GetThread()->Wait();
	}
}

void wxPDFViewPages::SetDocument(FPDF_DOCUMENT doc)
{
	m_doc = doc;
	clear();
	if (!doc)
		return;

	int pageCount = FPDF_GetPageCount(m_doc);
	reserve(pageCount);
	for (int i = 0; i < pageCount; ++i)
		push_back(wxPDFViewPage(this, i));
}

void wxPDFViewPages::RegisterClient(wxPDFViewPagesClient* client)
{
	m_clients.push_back(client);
}

void wxPDFViewPages::UnregisterClient(wxPDFViewPagesClient* client)
{
	std::remove(m_clients.begin(), m_clients.end(), client);
}

void wxPDFViewPages::RequestBitmapUpdate()
{
	// Notify render thread
	m_bmpUpdateHandlerCondition->Broadcast();
}

void wxPDFViewPages::NotifyPageUpdate(wxPDFViewPagesClient* client, int pageIndex)
{
	client->OnPageUpdated(pageIndex);
}

wxThread::ExitCode wxPDFViewPages::Entry()
{
	wxMutex requestHandlerMutex;
	requestHandlerMutex.Lock();
	m_bmpUpdateHandlerCondition = new wxCondition(requestHandlerMutex);

	while (m_bmpUpdateHandlerActive)
	{
		m_bmpUpdateHandlerCondition->Wait();

		// Check all pages for required bitmap updates and unload invisible pages data
		for (int pageIndex = 0; pageIndex < (int) size() && m_bmpUpdateHandlerActive; ++pageIndex)
		{
			wxPDFViewPage& page = (*this)[pageIndex];

			// Check page visibility and remove bitmap cache for invisible pages
			bool pageVisible = false;
			for (auto it = m_clients.begin(); it != m_clients.end(); ++it)
			{
				if ((*it)->IsPageVisible(pageIndex))
				{
					if (!pageVisible)
						pageVisible = true;

					// Check cache entry
					wxPDFViewPageBitmapCacheEntry& entry = (*it)->m_bitmapCache[pageIndex];
					if (entry.requiredSize.x > 0 && entry.requiredSize.y > 0 &&
						(!entry.bitmap.IsOk() || entry.requiredSize != entry.bitmap.GetSize()))
					{
						entry.bitmap = page.CreateCacheBitmap(entry.requiredSize);

						// Notify client to use the newly cached bitmap
						CallAfter(&wxPDFViewPages::NotifyPageUpdate, (*it), pageIndex);
					}
				} else
					(*it)->RemoveCachedBitmap(pageIndex);
			}

			if (!pageVisible)
				page.Unload();
		}
	}

	delete m_bmpUpdateHandlerCondition;
	m_bmpUpdateHandlerCondition = NULL;

	return 0;
}

//
// wxPDFViewPageClient
//

wxPDFViewPagesClient::wxPDFViewPagesClient()
{
	m_pPages = NULL;
	m_firstVisiblePage = -1;
	m_lastVisiblePage = -1;
}

void wxPDFViewPagesClient::SetPages(wxPDFViewPages* pages)
{
	if (m_pPages)
		m_pPages->UnregisterClient(this);
	m_pPages = pages;
	if (m_pPages)
		m_pPages->RegisterClient(this);
}

void wxPDFViewPagesClient::SetVisiblePages(int firstPage, int lastPage)
{
	if (!m_pPages)
		return;

	if (firstPage < 0)
		firstPage = 0;
	if (lastPage >= (int) m_pPages->size())
		lastPage = m_pPages->size() - 1;

	m_firstVisiblePage = firstPage;
	m_lastVisiblePage = lastPage;
}

bool wxPDFViewPagesClient::IsPageVisible(int pageIndex) const
{
	return pageIndex >= m_firstVisiblePage && pageIndex <= m_lastVisiblePage;
}

wxBitmap wxPDFViewPagesClient::GetCachedBitmap(int pageIndex, const wxSize& size)
{
	if (m_bitmapCache[pageIndex].requiredSize != size)
	{
		m_bitmapCache[pageIndex].requiredSize = size;
		m_pPages->RequestBitmapUpdate();
		return wxNullBitmap;
	} else
		return m_bitmapCache[pageIndex].bitmap;
}

void wxPDFViewPagesClient::RemoveCachedBitmap(int pageIndex)
{
	m_bitmapCache.erase(pageIndex);
}
