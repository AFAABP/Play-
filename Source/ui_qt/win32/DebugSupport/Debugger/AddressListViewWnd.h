#pragma once

#include "signal/Signal.h"
#include "win32/MDIChild.h"
#include "win32/ListBox.h"

class CAddressListViewWnd : public Framework::Win32::CMDIChild
{
public:
	typedef std::vector<uint32> AddressList;

	typedef Framework::CSignal<void(uint32)> AddressSelectedEvent;

	CAddressListViewWnd(HWND);
	virtual ~CAddressListViewWnd() = default;

	void SetTitle(std::tstring);
	void SetAddressList(AddressList);

	AddressSelectedEvent AddressSelected;

protected:
	long OnSize(unsigned int, unsigned int, unsigned int) override;
	long OnCommand(unsigned short, unsigned short, HWND) override;
	long OnSysCommand(unsigned int cmd, LPARAM) override;

private:
	void RefreshLayout();

	Framework::Win32::CListBox m_addressListBox;
};
