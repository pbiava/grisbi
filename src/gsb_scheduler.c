/* ************************************************************************** */
/*                                                                            */
/*     Copyright (C)	2000-2008 Cédric Auger (cedric@grisbi.org)            */
/*          2003-2008 Benjamin Drieu (bdrieu@april.org)	                      */
/*          https://www.grisbi.org                                            */
/*                                                                            */
/*  This program is free software; you can redistribute it and/or modify      */
/*  it under the terms of the GNU General Public License as published by      */
/*  the Free Software Foundation; either version 2 of the License, or         */
/*  (at your option) any later version.                                       */
/*                                                                            */
/*  This program is distributed in the hope that it will be useful,           */
/*  but WITHOUT ANY WARRANTY; without even the implied warranty of            */
/*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the             */
/*  GNU General Public License for more details.                              */
/*                                                                            */
/*  You should have received a copy of the GNU General Public License         */
/*  along with this program; if not, write to the Free Software               */
/*  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */
/*                                                                            */
/* ************************************************************************** */

/**
 * \file gsb_scheduler.c
 * contains several functions to work with the schedulers transactions
 */


#include "config.h"

/*START_INCLUDE*/
#include "gsb_scheduler.h"
#include "accueil.h"
#include "bet_finance_ui.h"
#include "grisbi_app.h"
#include "gsb_currency.h"
#include "gsb_data_account.h"
#include "gsb_data_fyear.h"
#include "gsb_data_payment.h"
#include "gsb_data_scheduled.h"
#include "gsb_data_transaction.h"
#include "gsb_form_scheduler.h"
#include "gsb_form_transaction.h"
#include "gsb_file.h"
#include "gsb_scheduler_list.h"
#include "gsb_transactions_list.h"
#include "structures.h"
#include "traitement_variables.h"
#include "utils_dates.h"
#include "utils_real.h"
#include "erreur.h"
/*END_INCLUDE*/

/*START_STATIC*/
/*END_STATIC*/

/*START_EXTERN*/
extern GSList *scheduled_transactions_taken;
extern GSList *scheduled_transactions_to_take;
/*END_EXTERN*/

/******************************************************************************/
/* Private functions                                                          */
/******************************************************************************/
/**
 * used to catch a transaction from a scheduled transaction
 * take the category, check if it's a transfer or a split and
 * do the necessary (create contra-transaction)
 * don't execute the children if it's a split, need to call
 * gsb_scheduler_execute_children_of_scheduled_transaction later
 *
 *
 * \param transaction_number
 * \param scheduled_number
 *
 * \return TRUE if ok, FALSE else
 **/
static gboolean gsb_scheduler_get_category_for_transaction_from_transaction (gint transaction_number,
																			 gint scheduled_number)
{
	/* if category is set, it's a normal category */
	if (gsb_data_scheduled_get_category_number (scheduled_number))
	{
		/* it's a normal category */
		gsb_data_transaction_set_category_number (transaction_number,
												  gsb_data_scheduled_get_category_number (scheduled_number));
		gsb_data_transaction_set_sub_category_number (transaction_number,
													  gsb_data_scheduled_get_sub_category_number (scheduled_number));

		return TRUE;
	}

	if (gsb_data_scheduled_get_split_of_scheduled (scheduled_number))
	{
		/* it's a split of transaction, */
		/* we don't append the children here, we need to call later */
		/* the function gsb_scheduler_execute_children_of_scheduled_transaction */
		gsb_data_transaction_set_split_of_transaction (transaction_number, 1);
	}
	else
	{
		gint contra_account_number;

		/* it's not a split of transaction and not a normal category so it's a transfer */
		/* except if the target account is -1 then it's a transaction with no category */
		contra_account_number = gsb_data_scheduled_get_account_number_transfer (scheduled_number);
		if (contra_account_number == 0)
		{
			GrisbiWinRun *w_run;

			w_run = grisbi_win_get_w_run ();
			if (w_run->account_number_is_0 == FALSE)
				return FALSE;
		}

		if (contra_account_number != -1)
		{
			gint contra_transaction_number;

			contra_transaction_number = gsb_form_transaction_validate_transfer (transaction_number,
																				TRUE,
																				gsb_data_scheduled_get_account_number_transfer
																				(scheduled_number));
			gsb_data_transaction_set_method_of_payment_number (contra_transaction_number,
															   gsb_data_scheduled_get_contra_method_of_payment_number
															   (scheduled_number));
		}
	}

	return TRUE;
}

/**
 * create a new transaction and fill it directly from a scheduled transaction
 * (don't pass throw the form)
 * if it's a child of split, append it automatickly to the mother
 *
 * \param scheduled_number the transaction we use to fill the new transaction
 * \param transaction_mother the number of the mother if it's a split child, 0 else
 *
 * \return the number of the new transaction
 **/
static gint gsb_scheduler_create_transaction_from_scheduled_transaction (gint scheduled_number,
																		 gint transaction_mother)
{
	GDate *date;
	gint account_number;
	gint payment_number;
	gint transfer_account;
	gint transaction_number;

	devel_debug_int (scheduled_number);
	account_number = gsb_data_scheduled_get_account_number (scheduled_number);

	transfer_account = gsb_data_scheduled_get_account_number_transfer (scheduled_number+1);
	transaction_number = gsb_data_transaction_new_transaction (account_number);

	/* initialise les données pour fixed date fix bug 2164: récupère la date de la transaction mère */
	if (transaction_mother)
	{
		date = gsb_data_scheduled_get_date (gsb_data_scheduled_get_mother_scheduled_number (scheduled_number));
	}
	else
		date = gsb_data_scheduled_get_date (scheduled_number);

	/* begin to fill the new transaction */
	gsb_data_transaction_set_date (transaction_number, date);

	gsb_data_transaction_set_payee_number (transaction_number,
										   gsb_data_scheduled_get_payee_number (scheduled_number));
	if (gsb_data_scheduled_get_split_of_scheduled (scheduled_number)
		&& gsb_data_account_get_bet_init_sch_with_loan (transfer_account))
	{
		GsbReal amount;

		amount = bet_finance_get_loan_amount_at_date (scheduled_number, transfer_account, date, TRUE);
		gsb_data_transaction_set_amount (transaction_number, amount);
	}
	else if (transaction_mother && transfer_account > 0)
	{
		gint scheduled_mother;

		if (bet_data_loan_get_last_loan_struct_by_account (transfer_account))
		{
			scheduled_mother = gsb_data_scheduled_get_mother_scheduled_number (scheduled_number);
			if (scheduled_mother == scheduled_number -1
				|| scheduled_mother == scheduled_number -2
				|| scheduled_mother == scheduled_number -3)
			{
				GsbReal amount;

				amount = bet_finance_get_loan_amount_at_date (scheduled_number,
															  transfer_account, date, FALSE);
				gsb_data_transaction_set_amount (transaction_number, amount);
			}
		}
		else
			gsb_data_transaction_set_amount (transaction_number,
											 gsb_data_scheduled_get_amount (scheduled_number));

	}
	else
		gsb_data_transaction_set_amount (transaction_number, gsb_data_scheduled_get_amount (scheduled_number));

	gsb_data_transaction_set_currency_number (transaction_number,
											  gsb_data_scheduled_get_currency_number (scheduled_number));
	gsb_data_transaction_set_account_number (transaction_number, account_number);

	/* ask for change if necessary, only for normal transaction ; a child must have the same currency number
	 * than the mother */
	if (!transaction_mother)
		gsb_currency_check_for_change (transaction_number);

	gsb_data_transaction_set_method_of_payment_number (transaction_number,
													   gsb_data_scheduled_get_method_of_payment_number
													   (scheduled_number));
	gsb_data_transaction_set_notes (transaction_number,
									gsb_data_scheduled_get_notes (scheduled_number));

	payment_number = gsb_data_scheduled_get_method_of_payment_number (scheduled_number);
	if (payment_number)
	{
		if (gsb_data_payment_get_show_entry (payment_number))
		{
			if (gsb_data_payment_get_automatic_numbering (payment_number))
			{
				gchar* tmpstr;

				tmpstr = gsb_data_payment_incremente_last_number (payment_number, 1);
				gsb_data_transaction_set_method_of_payment_content (transaction_number,
											 tmpstr);
				gsb_data_payment_set_last_number (payment_number, tmpstr);
				g_free (tmpstr);
			}
			else
				gsb_data_transaction_set_method_of_payment_content (transaction_number,
																	gsb_data_scheduled_get_method_of_payment_content
																	(scheduled_number));
		}
	}
	else
	{
	gsb_data_transaction_set_method_of_payment_content (transaction_number,
														gsb_data_scheduled_get_method_of_payment_content (scheduled_number));
	}
	gsb_data_transaction_set_automatic_transaction (transaction_number,
													gsb_data_scheduled_get_automatic_scheduled (scheduled_number));
	gsb_data_transaction_set_budgetary_number (transaction_number,
											   gsb_data_scheduled_get_budgetary_number (scheduled_number));
	gsb_data_transaction_set_sub_budgetary_number (transaction_number,
												   gsb_data_scheduled_get_sub_budgetary_number (scheduled_number));

	/* if the financial year is automatic, we set it here */
	if (gsb_data_scheduled_get_financial_year_number (scheduled_number) == 0)
		gsb_data_transaction_set_financial_year_number (transaction_number,
														gsb_data_fyear_get_from_date (gsb_data_transaction_get_date
																					  (transaction_number)));
	else
		gsb_data_transaction_set_financial_year_number (transaction_number,
														gsb_data_scheduled_get_financial_year_number (scheduled_number));

	/* get the category */
	gsb_scheduler_get_category_for_transaction_from_transaction (transaction_number, scheduled_number);

	 /* set the mother split if exists */
	gsb_data_transaction_set_mother_transaction_number (transaction_number, transaction_mother);

	/* we show the new transaction in the tree view */
	gsb_transactions_list_append_new_transaction (transaction_number, TRUE);

	return transaction_number;
}

/******************************************************************************/
/* Public functions                                                           */
/******************************************************************************/
/**
 * set the next date in the scheduled transaction
 * if it's above the limit date, that transaction is deleted
 * if it's a split, the children are updated too
 * if the scheduled transaction is finished, it's removed from the list and from the scheduled transactions
 *
 * \param scheduled_number the scheduled transaction we want to increase
 *
 * \return FALSE if the scheduled transaction is finished, TRUE else
 **/
gboolean gsb_scheduler_increase_scheduled (gint scheduled_number)
{
	GDate *new_date;

	g_return_val_if_fail (g_date_valid (gsb_data_scheduled_get_date (scheduled_number)), TRUE);

	/* increase the date of the scheduled_transaction */
	new_date = gsb_scheduler_get_next_date (scheduled_number,
											gsb_data_scheduled_get_date (scheduled_number));

	/* we continue to work only if new_date is not null (null mean reach the end) */
	if (new_date)
	{
		/* set the new date */
		gsb_data_scheduled_set_date (scheduled_number, new_date);

		if (gsb_data_scheduled_get_split_of_scheduled (scheduled_number))
		{
			GSList *children_numbers_list;
			gint transfer_account;
			gint init_sch_with_loan;
			GsbReal amount;

			transfer_account = gsb_data_scheduled_get_account_number_transfer (scheduled_number+1);
			init_sch_with_loan = gsb_data_account_get_bet_init_sch_with_loan (transfer_account);
			if (init_sch_with_loan)
			{
				amount = bet_finance_get_loan_amount_at_date (scheduled_number, transfer_account, new_date, FALSE);
				gsb_data_scheduled_set_amount (scheduled_number, amount);
			}

			/* if there is some children, set the new date too */
			children_numbers_list = gsb_data_scheduled_get_children (scheduled_number, TRUE);
			while (children_numbers_list)
			{
				gint child_number;

				child_number = GPOINTER_TO_INT (children_numbers_list->data);
				gsb_data_scheduled_set_date (child_number, new_date);
				if (init_sch_with_loan)
				{
					amount = bet_finance_get_loan_amount_at_date (child_number, transfer_account, new_date, FALSE);
					gsb_data_scheduled_set_amount (child_number, amount);
				}

				children_numbers_list = children_numbers_list->next;
			}
			g_slist_free (children_numbers_list);
		}
		g_date_free (new_date);
	}
	else
	{
		/* the scheduled transaction is over, we remove it */
		/* update the main page */
		gsb_main_page_update_finished_scheduled_transactions (scheduled_number);

		/* remove the scheduled transaction and children (bug 2255 */
		if (gsb_data_scheduled_get_split_of_scheduled (scheduled_number))
		{
			gsb_data_scheduled_remove_child_scheduled (scheduled_number);
		}
		gsb_data_scheduled_remove_scheduled (scheduled_number);

		return FALSE;
	}
	return TRUE;
}

/**
 * find and return the next date after the given date for the given scheduled
 * transaction
 *
 * \param scheduled_number
 * \param date the current date, we want the next one after that one
 *
 * \return a newly allocated date, the next date or NULL if over the limit
 **/
GDate *gsb_scheduler_get_next_date (gint scheduled_number,
									const GDate *date)
{
	GDate *tmp_date;
	GDate *return_date;
	gint fixed_date;

	if (!scheduled_number || !gsb_data_scheduled_get_frequency (scheduled_number)
		|| !date || !g_date_valid (date))
		return NULL;

	/* we don't change the initial date */
	return_date = gsb_date_copy (date);

	/* initialise les données pour fixed date */
	fixed_date = gsb_data_scheduled_get_fixed_date (scheduled_number);

	switch (gsb_data_scheduled_get_frequency (scheduled_number))
	{
		case SCHEDULER_PERIODICITY_ONCE_VIEW:
			return NULL;
			break;

		case SCHEDULER_PERIODICITY_WEEK_VIEW:
			g_date_add_days (return_date, 7);
			break;

		case SCHEDULER_PERIODICITY_MONTH_VIEW:
			g_date_add_months (return_date, 1);

			/* set the correct date if necessary */
			if (fixed_date)
			{
				tmp_date = gsb_date_copy (return_date);
				g_date_set_day (return_date, fixed_date);
				if (!g_date_valid (return_date))
				{
					g_date_free (return_date);
					return_date = gsb_date_get_last_day_of_month (tmp_date);
				}
				g_date_free (tmp_date);
			}
			break;

		case SCHEDULER_PERIODICITY_TWO_MONTHS_VIEW:
			g_date_add_months (return_date, 2);

			/* set the correct date if necessary */
			if (fixed_date)
			{
				tmp_date = gsb_date_copy (return_date);
				g_date_set_day (return_date, fixed_date);
				if (!g_date_valid (return_date))
				{
					g_date_free (return_date);
					return_date = gsb_date_get_last_day_of_month (tmp_date);
				}
				g_date_free (tmp_date);
			}
			break;

		case SCHEDULER_PERIODICITY_TRIMESTER_VIEW:
			g_date_add_months (return_date, 3);

			/* set the correct date if necessary */
			if (fixed_date)
			{
				tmp_date = gsb_date_copy (return_date);
				g_date_set_day (return_date, fixed_date);
				if (!g_date_valid (return_date))
				{
					g_date_free (return_date);
					return_date = gsb_date_get_last_day_of_month (tmp_date);
				}
				g_date_free (tmp_date);
			}
			break;

		case SCHEDULER_PERIODICITY_YEAR_VIEW:
			g_date_add_years (return_date, 1);
			break;

		case SCHEDULER_PERIODICITY_CUSTOM_VIEW:
			if (gsb_data_scheduled_get_user_entry (scheduled_number) <= 0)
			{
				g_date_free (return_date);

				return NULL;
			}

			switch (gsb_data_scheduled_get_user_interval (scheduled_number))
			{
				case PERIODICITY_DAYS:
					g_date_add_days (return_date,
									 gsb_data_scheduled_get_user_entry (scheduled_number));
					break;

				case PERIODICITY_WEEKS:
					g_date_add_days (return_date,
							  gsb_data_scheduled_get_user_entry (scheduled_number) * 7);
					g_date_add_months (return_date, 0);
					break;

				case PERIODICITY_MONTHS:
					g_date_add_months (return_date,
									   gsb_data_scheduled_get_user_entry (scheduled_number));

					/* set the correct date if necessary */
					if (fixed_date)
					{
						tmp_date = gsb_date_copy (return_date);
						g_date_set_day (return_date, fixed_date);
						if (!g_date_valid (return_date))
						{
							g_date_free (return_date);
							return_date = gsb_date_get_last_day_of_month (tmp_date);
						}
						g_date_free (tmp_date);
					}
					break;

				case PERIODICITY_YEARS:
					g_date_add_years (return_date,
									  gsb_data_scheduled_get_user_entry (scheduled_number));
					g_date_add_months (return_date, 0);
					break;
			}
			break;
	}

	if (gsb_data_scheduled_get_limit_date (scheduled_number)
		&& g_date_compare (return_date, gsb_data_scheduled_get_limit_date (scheduled_number)) > 0)
	{
		g_date_free (return_date);
		return_date = NULL;
	}

	return (return_date);
}

/**
 * get the children of a split scheduled transaction,
 * make the transactions from them and append them to the transactions list
 *
 *
 * \param scheduled_number the number of the mother scheduled transaction (the split)
 * \param transaction_number the number of the transaction created from that scheduled (so, the future mother of the children)
 *
 * \return FALSE
 *
 **/
gboolean gsb_scheduler_execute_children_of_scheduled_transaction (gint scheduled_number,
																  gint transaction_number)
{
	GSList *children_numbers_list;

	children_numbers_list = gsb_data_scheduled_get_children (scheduled_number, TRUE);
	while (children_numbers_list)
	{
		gint child_number;

		child_number = GPOINTER_TO_INT (children_numbers_list->data);

		/* pbiava the 03/16/2009 supprime le crash quand on execute la transaction
		 * a partir du planificateur risque d'effet de bord */
		if (child_number > 0)
			gsb_scheduler_create_transaction_from_scheduled_transaction (child_number, transaction_number);

		children_numbers_list = children_numbers_list->next;
	}

	g_slist_free (children_numbers_list);

	return FALSE;
}

/**
 * update the children of a split scheduled transaction,
 *
 * \param scheduled_number the number of the mother scheduled transaction (the split)
 *
 * \return FALSE
 *
 **/
void gsb_scheduler_update_children_from_split_scheduled (gint scheduled_number)
{
	GSList *children_numbers_list;

	children_numbers_list = gsb_data_scheduled_get_children (scheduled_number, TRUE);
	while (children_numbers_list)
	{
		gint child_number;

		child_number = GPOINTER_TO_INT (children_numbers_list->data);
		if (child_number > 0)
		{
			gsb_form_scheduler_set_from_mother (child_number, scheduled_number);
		}

		children_numbers_list = children_numbers_list->next;
	}
	g_slist_free (children_numbers_list);
}

/**
 * check the scheduled transactions if the are in time limit
 * and record the automatic transactions
 *
 * \param
 *
 * \return
 **/
void gsb_scheduler_check_scheduled_transactions_time_limit (void)
{
	GDate *date;
	GSList *tmp_list;
	gboolean automatic_transactions_taken = FALSE;
	GrisbiAppConf *a_conf;
	GrisbiWinRun *w_run;

	devel_debug (NULL);
	a_conf = (GrisbiAppConf *) grisbi_app_get_a_conf ();
	w_run = (GrisbiWinRun *) grisbi_win_get_w_run ();

	/* the scheduled transactions to take will be check here, */
	/* but the scheduled transactions taken will be add to the already appended ones */
	scheduled_transactions_to_take = NULL;

	/* get the date today + a_conf->nb_days_before_scheduled */
	/* the date untill we execute the scheduled transactions is : */
	/* - either today + a_conf->nb_days_before_scheduled if warn n days before the scheduled */
	/* - either the end of the month in a_conf->nb_days_before_scheduled days (so current month or next month) */
	/*   or the fixed date */
	date = gdate_today ();

	/* now date is in a_conf->nb_days_before_scheduled, if we want the transactions of the month, */
	/* we change date to the end of its month */
	if (a_conf->execute_scheduled_of_month)
	{
		gint last_day;

		if (a_conf->scheduler_set_fixed_day && g_date_get_day (date) >= a_conf->scheduler_fixed_day)
		{
			g_date_add_months (date, 1);
		}

		last_day = g_date_get_days_in_month (g_date_get_month (date), g_date_get_year (date));
		g_date_set_day (date, last_day);
	}
	else
	{
		g_date_add_days (date, a_conf->nb_days_before_scheduled);
	}

	/* check all the scheduled transactions, */
	/* if automatic, it's taken */
	/* if manual, appended into scheduled_transactions_to_take */
	tmp_list = gsb_data_scheduled_get_scheduled_list ();

	while (tmp_list)
	{
		gint scheduled_number;

		scheduled_number = gsb_data_scheduled_get_scheduled_number (tmp_list->data);

		/* we check that scheduled transaction only if it's not a child of a split */
		if (!gsb_data_scheduled_get_mother_scheduled_number (scheduled_number)
			 && gsb_data_scheduled_get_date (scheduled_number)
			 && g_date_compare (gsb_data_scheduled_get_date (scheduled_number), date) <= 0)
		{
			if (gsb_data_scheduled_get_automatic_scheduled (scheduled_number))
			{
				/* this is an automatic scheduled, we get it */
				gint transaction_number;

				/* take automatically the scheduled transaction untill today */
				transaction_number = gsb_scheduler_create_transaction_from_scheduled_transaction (scheduled_number, 0);
				if (gsb_data_scheduled_get_split_of_scheduled (scheduled_number))
					gsb_scheduler_execute_children_of_scheduled_transaction (scheduled_number, transaction_number);

				scheduled_transactions_taken = g_slist_append (scheduled_transactions_taken,
															   GINT_TO_POINTER (transaction_number));
				automatic_transactions_taken = TRUE;

				/* set the scheduled transaction to the next date, */
				/* if it's not finished, we check them again if it need to be */
				/* executed more than one time (the easiest way is to check */
				/* all again, i don't think it will have thousand of scheduled transactions, */
				/* so no much waste of time...) */

				/* fixe probablement le bug bug 2255 */
				if (gsb_scheduler_increase_scheduled (scheduled_number))
					scheduled_transactions_to_take = NULL;

				tmp_list = gsb_data_scheduled_get_scheduled_list ();
			}
			else
			{
				/* it's a manual scheduled transaction, we put it in the slist */
				scheduled_transactions_to_take = g_slist_append (scheduled_transactions_to_take ,
																 GINT_TO_POINTER (scheduled_number));
				tmp_list = tmp_list->next;
			}
		}
		else
			tmp_list = tmp_list->next;
	}

	if (automatic_transactions_taken)
	{
		w_run->mise_a_jour_liste_echeances_auto_accueil = TRUE;
		gsb_file_set_modified (TRUE);
	}

	if (scheduled_transactions_to_take)
		w_run->mise_a_jour_liste_echeances_manuelles_accueil = TRUE;

	g_date_free (date);
}
/**
 *
 *
 * \param
 *
 * \return
 **/
/* Local Variables: */
/* c-basic-offset: 4 */
/* End: */
