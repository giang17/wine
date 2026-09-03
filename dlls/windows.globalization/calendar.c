/* WinRT Windows.Globalization.Calendar implementation
 *
 * Copyright 2026 Giang Nguyen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(locale);

struct calendar
{
    ICalendar ICalendar_iface;
    LONG ref;
};

static inline struct calendar *impl_from_ICalendar( ICalendar *iface )
{
    return CONTAINING_RECORD( iface, struct calendar, ICalendar_iface );
}

static HRESULT WINAPI calendar_QueryInterface( ICalendar *iface, REFIID iid, void **out )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_ICalendar ))
    {
        IInspectable_AddRef( (*out = &impl->ICalendar_iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI calendar_AddRef( ICalendar *iface )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI calendar_Release( ICalendar *iface )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI calendar_GetIids( ICalendar *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_GetRuntimeClassName( ICalendar *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p.\n", iface, class_name );
    return WindowsCreateString( RuntimeClass_Windows_Globalization_Calendar,
                                ARRAY_SIZE(RuntimeClass_Windows_Globalization_Calendar) - 1, class_name );
}

static HRESULT WINAPI calendar_GetTrustLevel( ICalendar *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_Clone( ICalendar *iface, ICalendar **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_SetToMin( ICalendar *iface )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_SetToMax( ICalendar *iface )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_Languages( ICalendar *iface, IVectorView_HSTRING **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_NumeralSystem( ICalendar *iface, HSTRING *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_put_NumeralSystem( ICalendar *iface, HSTRING value )
{
    FIXME( "iface %p, value %s stub!\n", iface, debugstr_hstring( value ) );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_GetCalendarSystem( ICalendar *iface, HSTRING *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_ChangeCalendarSystem( ICalendar *iface, HSTRING value )
{
    FIXME( "iface %p, value %s stub!\n", iface, debugstr_hstring( value ) );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_GetClock( ICalendar *iface, HSTRING *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_ChangeClock( ICalendar *iface, HSTRING value )
{
    FIXME( "iface %p, value %s stub!\n", iface, debugstr_hstring( value ) );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_GetDateTime( ICalendar *iface, DateTime *result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_SetDateTime( ICalendar *iface, DateTime value )
{
    FIXME( "iface %p, value %I64d stub!\n", iface, value.UniversalTime );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_SetToNow( ICalendar *iface )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_FirstEra( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_LastEra( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_NumberOfEras( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_Era( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_put_Era( ICalendar *iface, INT32 value )
{
    FIXME( "iface %p, value %d stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_AddEras( ICalendar *iface, INT32 eras )
{
    FIXME( "iface %p, eras %d stub!\n", iface, eras );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_EraAsFullString( ICalendar *iface, HSTRING *result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_EraAsString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    FIXME( "iface %p, ideal_length %d, result %p stub!\n", iface, ideal_length, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_FirstYearInThisEra( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_LastYearInThisEra( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_NumberOfYearsInThisEra( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_Year( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_put_Year( ICalendar *iface, INT32 value )
{
    FIXME( "iface %p, value %d stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_AddYears( ICalendar *iface, INT32 years )
{
    FIXME( "iface %p, years %d stub!\n", iface, years );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_YearAsString( ICalendar *iface, HSTRING *result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_YearAsTruncatedString( ICalendar *iface, INT32 remaining_digits, HSTRING *result )
{
    FIXME( "iface %p, remaining_digits %d, result %p stub!\n", iface, remaining_digits, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_YearAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    FIXME( "iface %p, min_digits %d, result %p stub!\n", iface, min_digits, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_FirstMonthInThisYear( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_LastMonthInThisYear( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_NumberOfMonthsInThisYear( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_Month( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_put_Month( ICalendar *iface, INT32 value )
{
    FIXME( "iface %p, value %d stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_AddMonths( ICalendar *iface, INT32 months )
{
    FIXME( "iface %p, months %d stub!\n", iface, months );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_MonthAsFullString( ICalendar *iface, HSTRING *result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_MonthAsString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    FIXME( "iface %p, ideal_length %d, result %p stub!\n", iface, ideal_length, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_MonthAsFullSoloString( ICalendar *iface, HSTRING *result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_MonthAsSoloString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    FIXME( "iface %p, ideal_length %d, result %p stub!\n", iface, ideal_length, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_MonthAsNumericString( ICalendar *iface, HSTRING *result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_MonthAsPaddedNumericString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    FIXME( "iface %p, min_digits %d, result %p stub!\n", iface, min_digits, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_AddWeeks( ICalendar *iface, INT32 weeks )
{
    FIXME( "iface %p, weeks %d stub!\n", iface, weeks );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_FirstDayInThisMonth( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_LastDayInThisMonth( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_NumberOfDaysInThisMonth( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_Day( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_put_Day( ICalendar *iface, INT32 value )
{
    FIXME( "iface %p, value %d stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_AddDays( ICalendar *iface, INT32 days )
{
    FIXME( "iface %p, days %d stub!\n", iface, days );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_DayAsString( ICalendar *iface, HSTRING *result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_DayAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    FIXME( "iface %p, min_digits %d, result %p stub!\n", iface, min_digits, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_DayOfWeek( ICalendar *iface, DayOfWeek *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_DayOfWeekAsFullString( ICalendar *iface, HSTRING *result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_DayOfWeekAsString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    FIXME( "iface %p, ideal_length %d, result %p stub!\n", iface, ideal_length, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_DayOfWeekAsFullSoloString( ICalendar *iface, HSTRING *result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_DayOfWeekAsSoloString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    FIXME( "iface %p, ideal_length %d, result %p stub!\n", iface, ideal_length, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_FirstPeriodInThisDay( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_LastPeriodInThisDay( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_NumberOfPeriodsInThisDay( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_Period( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_put_Period( ICalendar *iface, INT32 value )
{
    FIXME( "iface %p, value %d stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_AddPeriods( ICalendar *iface, INT32 periods )
{
    FIXME( "iface %p, periods %d stub!\n", iface, periods );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_PeriodAsFullString( ICalendar *iface, HSTRING *result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_PeriodAsString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    FIXME( "iface %p, ideal_length %d, result %p stub!\n", iface, ideal_length, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_FirstHourInThisPeriod( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_LastHourInThisPeriod( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_NumberOfHoursInThisPeriod( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_Hour( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_put_Hour( ICalendar *iface, INT32 value )
{
    FIXME( "iface %p, value %d stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_AddHours( ICalendar *iface, INT32 hours )
{
    FIXME( "iface %p, hours %d stub!\n", iface, hours );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_HourAsString( ICalendar *iface, HSTRING *result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_HourAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    FIXME( "iface %p, min_digits %d, result %p stub!\n", iface, min_digits, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_Minute( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_put_Minute( ICalendar *iface, INT32 value )
{
    FIXME( "iface %p, value %d stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_AddMinutes( ICalendar *iface, INT32 minutes )
{
    FIXME( "iface %p, minutes %d stub!\n", iface, minutes );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_MinuteAsString( ICalendar *iface, HSTRING *result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_MinuteAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    FIXME( "iface %p, min_digits %d, result %p stub!\n", iface, min_digits, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_Second( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_put_Second( ICalendar *iface, INT32 value )
{
    FIXME( "iface %p, value %d stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_AddSeconds( ICalendar *iface, INT32 seconds )
{
    FIXME( "iface %p, seconds %d stub!\n", iface, seconds );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_SecondAsString( ICalendar *iface, HSTRING *result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_SecondAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    FIXME( "iface %p, min_digits %d, result %p stub!\n", iface, min_digits, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_Nanosecond( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_put_Nanosecond( ICalendar *iface, INT32 value )
{
    FIXME( "iface %p, value %d stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_AddNanoseconds( ICalendar *iface, INT32 nanoseconds )
{
    FIXME( "iface %p, nanoseconds %d stub!\n", iface, nanoseconds );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_NanosecondAsString( ICalendar *iface, HSTRING *result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_NanosecondAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    FIXME( "iface %p, min_digits %d, result %p stub!\n", iface, min_digits, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_Compare( ICalendar *iface, ICalendar *other, INT32 *result )
{
    FIXME( "iface %p, other %p, result %p stub!\n", iface, other, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_CompareDateTime( ICalendar *iface, DateTime other, INT32 *result )
{
    FIXME( "iface %p, other %I64d, result %p stub!\n", iface, other.UniversalTime, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_CopyTo( ICalendar *iface, ICalendar *other )
{
    FIXME( "iface %p, other %p stub!\n", iface, other );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_FirstMinuteInThisHour( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_LastMinuteInThisHour( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_NumberOfMinutesInThisHour( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_FirstSecondInThisMinute( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_LastSecondInThisMinute( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_NumberOfSecondsInThisMinute( ICalendar *iface, INT32 *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_ResolvedLanguage( ICalendar *iface, HSTRING *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_get_IsDaylightSavingTime( ICalendar *iface, boolean *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static const struct ICalendarVtbl calendar_vtbl =
{
    calendar_QueryInterface,
    calendar_AddRef,
    calendar_Release,
    /* IInspectable methods */
    calendar_GetIids,
    calendar_GetRuntimeClassName,
    calendar_GetTrustLevel,
    /* ICalendar methods */
    calendar_Clone,
    calendar_SetToMin,
    calendar_SetToMax,
    calendar_get_Languages,
    calendar_get_NumeralSystem,
    calendar_put_NumeralSystem,
    calendar_GetCalendarSystem,
    calendar_ChangeCalendarSystem,
    calendar_GetClock,
    calendar_ChangeClock,
    calendar_GetDateTime,
    calendar_SetDateTime,
    calendar_SetToNow,
    calendar_get_FirstEra,
    calendar_get_LastEra,
    calendar_get_NumberOfEras,
    calendar_get_Era,
    calendar_put_Era,
    calendar_AddEras,
    calendar_EraAsFullString,
    calendar_EraAsString,
    calendar_get_FirstYearInThisEra,
    calendar_get_LastYearInThisEra,
    calendar_get_NumberOfYearsInThisEra,
    calendar_get_Year,
    calendar_put_Year,
    calendar_AddYears,
    calendar_YearAsString,
    calendar_YearAsTruncatedString,
    calendar_YearAsPaddedString,
    calendar_get_FirstMonthInThisYear,
    calendar_get_LastMonthInThisYear,
    calendar_get_NumberOfMonthsInThisYear,
    calendar_get_Month,
    calendar_put_Month,
    calendar_AddMonths,
    calendar_MonthAsFullString,
    calendar_MonthAsString,
    calendar_MonthAsFullSoloString,
    calendar_MonthAsSoloString,
    calendar_MonthAsNumericString,
    calendar_MonthAsPaddedNumericString,
    calendar_AddWeeks,
    calendar_get_FirstDayInThisMonth,
    calendar_get_LastDayInThisMonth,
    calendar_get_NumberOfDaysInThisMonth,
    calendar_get_Day,
    calendar_put_Day,
    calendar_AddDays,
    calendar_DayAsString,
    calendar_DayAsPaddedString,
    calendar_get_DayOfWeek,
    calendar_DayOfWeekAsFullString,
    calendar_DayOfWeekAsString,
    calendar_DayOfWeekAsFullSoloString,
    calendar_DayOfWeekAsSoloString,
    calendar_get_FirstPeriodInThisDay,
    calendar_get_LastPeriodInThisDay,
    calendar_get_NumberOfPeriodsInThisDay,
    calendar_get_Period,
    calendar_put_Period,
    calendar_AddPeriods,
    calendar_PeriodAsFullString,
    calendar_PeriodAsString,
    calendar_get_FirstHourInThisPeriod,
    calendar_get_LastHourInThisPeriod,
    calendar_get_NumberOfHoursInThisPeriod,
    calendar_get_Hour,
    calendar_put_Hour,
    calendar_AddHours,
    calendar_HourAsString,
    calendar_HourAsPaddedString,
    calendar_get_Minute,
    calendar_put_Minute,
    calendar_AddMinutes,
    calendar_MinuteAsString,
    calendar_MinuteAsPaddedString,
    calendar_get_Second,
    calendar_put_Second,
    calendar_AddSeconds,
    calendar_SecondAsString,
    calendar_SecondAsPaddedString,
    calendar_get_Nanosecond,
    calendar_put_Nanosecond,
    calendar_AddNanoseconds,
    calendar_NanosecondAsString,
    calendar_NanosecondAsPaddedString,
    calendar_Compare,
    calendar_CompareDateTime,
    calendar_CopyTo,
    calendar_get_FirstMinuteInThisHour,
    calendar_get_LastMinuteInThisHour,
    calendar_get_NumberOfMinutesInThisHour,
    calendar_get_FirstSecondInThisMinute,
    calendar_get_LastSecondInThisMinute,
    calendar_get_NumberOfSecondsInThisMinute,
    calendar_get_ResolvedLanguage,
    calendar_get_IsDaylightSavingTime,
};

struct calendar_factory
{
    IActivationFactory IActivationFactory_iface;
    LONG ref;
};

static inline struct calendar_factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct calendar_factory, IActivationFactory_iface );
}

static HRESULT WINAPI activation_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct calendar_factory *factory = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef( (*out = &factory->IActivationFactory_iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI activation_factory_AddRef( IActivationFactory *iface )
{
    struct calendar_factory *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI activation_factory_Release( IActivationFactory *iface )
{
    struct calendar_factory *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI activation_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI activation_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI activation_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI activation_factory_ActivateInstance( IActivationFactory *iface, IInspectable **out )
{
    struct calendar *calendar;

    FIXME( "iface %p, out %p semi-stub.\n", iface, out );

    if (!(calendar = calloc( 1, sizeof(*calendar) ))) return E_OUTOFMEMORY;
    calendar->ICalendar_iface.lpVtbl = &calendar_vtbl;
    calendar->ref = 1;

    *out = (IInspectable *)&calendar->ICalendar_iface;
    return S_OK;
}

static const struct IActivationFactoryVtbl activation_factory_vtbl =
{
    activation_factory_QueryInterface,
    activation_factory_AddRef,
    activation_factory_Release,
    /* IInspectable methods */
    activation_factory_GetIids,
    activation_factory_GetRuntimeClassName,
    activation_factory_GetTrustLevel,
    /* IActivationFactory methods */
    activation_factory_ActivateInstance,
};

static struct calendar_factory factory =
{
    {&activation_factory_vtbl},
    1,
};

IActivationFactory *calendar_factory = &factory.IActivationFactory_iface;
