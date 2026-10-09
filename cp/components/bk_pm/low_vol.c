// Copyright 2020-2023 Beken
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <modules/pm.h>
#include <driver/pwr_clk.h>

/*=====================DEFINE  SECTION  START=====================*/

/*=====================DEFINE  SECTION  END=======================*/

/*=====================VARIABLE  SECTION  START===================*/
//static volatile  uint64_t s_pm_module_lv_sleep_state               = 0;


/*=====================VARIABLE  SECTION  END=====================*/

/*================FUNCTION DECLARATION  SECTION  START============*/


/*================FUNCTION DECLARATION  SECTION  END===============*/
uint64_t bk_pm_module_lv_sleep_state_get(pm_dev_id_e module)
{
	return pm_shared_u64_bit_get(
		&FIXED_ADDR_PM_MODULE_LV_SLEEP_STATE, (uint32_t)module);
}
__attribute__((section(".itcm_sec_code"))) bk_err_t bk_pm_module_lv_sleep_state_set()
{
	return pm_shared_u64_store(
		&FIXED_ADDR_PM_MODULE_LV_SLEEP_STATE, UINT64_MAX)
		? BK_OK : BK_FAIL;
}
bk_err_t bk_pm_module_lv_sleep_state_clear(pm_dev_id_e module)
{
	return pm_shared_u64_bit_clear(
		&FIXED_ADDR_PM_MODULE_LV_SLEEP_STATE, (uint32_t)module)
		? BK_OK : BK_FAIL;
}

bk_err_t pm_debug_lv_state()
{
	BK_LOGI(NULL,"pm_module_lv_sleep_state:0x%llx\r\n",
		pm_shared_u64_load(&FIXED_ADDR_PM_MODULE_LV_SLEEP_STATE));
	return BK_OK;
}