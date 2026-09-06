window.onload=function()
{
	//判断是否之前选择了语言，如果已选择则显示默认语言
	if(localStorage.getItem('lang'))
		slang.value=localStorage.getItem('lang');
	else
	{
		//如果之前未选择语言，则自动判断当前语言
		if(navigator.language.startsWith('zh'))
		{
		}
		else
			slang.value='en';
	}
	//更改语言
	slang.onchange=function()
	{
		// location.href=location.href.replace(/(\/zh\/)|(\/en\/)/,'/'+this.value+'/');
		localStorage.setItem('lang',this.value);
		// location.reload();
		langChange();
	}
	//显示语言
	function langChange()
	{
		let nlang=localStorage.getItem('lang');
		if(nlang=='en')
		{
			$js('[lang=zh]').hide();
			$js('[lang=en]').show();
			main.$js('[lang=zh]').hide();
			main.$js('[lang=en]').show();
		}
		else
		{
			$js('[lang=zh]').show();
			$js('[lang=en]').hide();
			main.$js('[lang=zh]').show();
			main.$js('[lang=en]').hide();
		}
		// console.log(main);
		// main.langChange();
	}
	langChange();
}